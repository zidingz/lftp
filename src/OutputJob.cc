/*
 * lftp and utils
 *
 * Copyright (c) 2002 by Alexander V. Lukyanov (lav@yars.free.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Usage notes:
 *
 * Call PreFilter() to add a filter to the beginning of the chain; these
 * filters are initialized only once for all data.  For example,
 * PreFilter("wc -l")
 *
 */

/*
 * Implementation notes:
 *
 * Background things we can't get around: 
 * We must buffer (via FileCopy) output to a filter, since it might block.
 *
 * We must buffer the output from the filter to an output FileCopyPeer (ie.
 * a URL), for the same reason.
 *
 * So, we're stuck with having up to two FileCopy's.  (One to send, one to filter.)
 *
 * In some cases, we only need one: if the output is an FD, the filter can
 * hook up directly and we can forget about that stage.
 *
 * In the case where we're outputting to a URL, we set up a FileCopy from a
 * pipe to the URL, and then pretend we're just outputting to an FD (the
 * pipe.) This means in the simple case of having no filters at all, writing
 * to a URL or file, we send the data an extra time through a FileCopy and a
 * pipe.  That's a bit inefficient, but that's "cat file1 > file2"; that's
 * normally done with "get file1 -o file2", so this shouldn't happen often.
 *
 * It's very important that if the output is stdout, any filters point directly
 * at it, not through an extra copy: a pager, for example, will expect the output
 * to be a TTY.
 *
 */
#include <config.h>

#include "OutputJob.h"
#include "ArgV.h"
#include "FileCopy.h"
#include "CopyJob.h"
#include "url.h"
#include "misc.h"
#include "StatusLine.h"
#include "LocalAccess.h"

#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define super Job

void OutputJob::InitCopy()
{
   if(error)
      return;

   if(initialized)
      return;

   initialized=true;

   if(Error())
      return;

   /* Clear the statusline, since we might change the pgrp if we create filters. */
   printf("%s", ""); /* (and avoid gcc warning) */

   /* Some legitimate uses produce broken pipe condition (cat|head).
    * We still want to produce broken pipe if we're not piping, eg
    * cat > pipe. */
   if(IsFiltered())
      fail_if_broken=false;

   if(filter)
   {
      /* Create the global filter: */
      OutputFilter *global = new OutputFilter(filter, output_fd);
      global->DeleteSecondaryStream();
      output_fd=global;
   }

   /* Use a FileCopy to buffer our output to the filter: */
   FileCopyPeerFDStream *out = new FileCopyPeerFDStream(output_fd, FileCopyPeer::PUT);
   FileCopy *input_fc = FileCopy::New(new FileCopyPeer(FileCopyPeer::GET), out, false);

   /* out now owns output_fd, and will delete it when it's finished, so
    * we can't keep it around. */
   output_fd=0;

   // I don't think we need to do this; the CopyJob picks up the output_fd's
   // FgData now, since we're not telling it not to delete it.
   // fg_data=new FgData(output_fd->GetProcGroup(),fg);

   if(!fail_if_broken)
      input_fc->DontFailIfBroken();

   char *buf = xasprintf(_("%s (filter)"), a0);
   input=new CopyJob(input_fc, buf, filter?filter:a0);
   xfree(buf);

   if(!output)
      output=input;

   input->SetParentFg(this);
   InputPeer()->SetDate(NO_DATE);
   InputPeer()->SetSize(NO_SIZE);
   input->GetCopy()->DontCopyDate();
   input->NoStatus();

   if(input != output)
   {
      output->SetParentFg(this);
      OutputPeer()->SetDate(NO_DATE);
      OutputPeer()->SetSize(NO_SIZE);
      output->GetCopy()->DontCopyDate();
      output->NoStatus();
   }

   if(is_stdout)
   {
      output->ClearStatusOnWrite();
      output->GetCopy()->LineBuffered();
   }

   Timeout(0);
}

void OutputJob::Init(const char *_a0)
{
   input=output=0;
   filter=0;
   initialized=false;
   error=false;
   no_status=false;
   a0=xstrdup(_a0);
   is_stdout=false;
   fail_if_broken=true;
   output_fd=0;
   is_a_tty=false;
   width=-1;
   statusbar_redisplay=true;
}

/* Local (fd) output. */
OutputJob::OutputJob(FDStream *output_, const char *a0)
{
   Init(a0);

   output_fd=output_;

   if(!output_fd)
      output_fd=new FDStream(1,"<stdout>");
   else
      /* We don't want to produce broken pipe when we're actually
       * piping, since some legitimate uses produce broken pipe, eg
       * cat|head.  However, that's actually handled in InitCopy().
       * User pipes aren't handled by us yet: instead of being set with
       * SetFilter, they're being set up ahead of time and passed to
       * us as an FDStream, so we don't really know if we're being filtered.
       * 
       * So, until we handle pipes directly, disable broken pipe whenever
       * we're being sent anywhere but stdout. */
      fail_if_broken=false;

   is_stdout=output_fd->usesfd(1);
   is_a_tty=isatty(output_fd->fd);
   width=fd_width(output_fd->fd);

   /* We don't output status when outputting locally. */
   no_status=true;

   /* Make sure that if the output is going to fail, it fails early, so
    * the parent doesn't start anything expensive (like begin downloading
    * a file.) */
   if(output_fd->getfd() == -1)
   {
      eprintf("%s: %s\n", a0, output_fd->error_text);
      error=true;
   }
}

OutputJob::OutputJob(const char *path, const char *a0, FileAccess *fa)
{
   Init(a0);

   /* Set up a pipe sending data at the peer, so we can act like the FDStream
    * constructor. */
   int filter_pipe[2];

   if(pipe(filter_pipe) == -1) {
      /* FIXME: This can be retryable. */
      eprintf("%s: %s\n", a0, strerror(errno));
      error=true;
      return;
   }

   bool reuse = false;
   if(!fa)
   {
      fa = new LocalAccess;
      reuse = true;
   }

   FileCopyPeerFA *dst_peer = FileCopyPeerFA::New(fa, path, FA::STORE, reuse);

   /* Status only for remote outputs. */
   if(!strcmp(dst_peer->GetProto(), "file"))
      no_status=true;

   fcntl(filter_pipe[0],F_SETFL,O_NONBLOCK);
   fcntl(filter_pipe[1],F_SETFL,O_NONBLOCK);

   /* The output of the pipe (0) goes to the output FileCopy. */
   FDStream *pipe_output = new FDStream(filter_pipe[0],"<filter-out>");

   FileCopy *output_fc=FileCopy::New(new FileCopyPeerFDStream(pipe_output, FileCopyPeer::GET), dst_peer,false);
   output=new CopyJob(output_fc, path, a0);

   output_fd=new FDStream(filter_pipe[1],"<filter-in>");

   pipe_output->CloseFD();
   output_fd->CloseFD();
}

OutputJob::~OutputJob()
{
   Bg();
   AcceptSig(SIGTERM);

   Delete(input);
   if(input != output)
      Delete(output);
   delete output_fd;

   xfree(a0);
   xfree(filter);
}

/* This is called to ask us "permission" to display a status line. */
bool OutputJob::ShowStatusLine(StatusLine *s)
{
   /* If our output file is gone, or isn't stdout, we don't care. */
   if(!output || !is_stdout)
      return true;

   /* If we're filtered, we never display at all.  (We don't know anything about
    * the filter's output; the only time we do is when we're outputting to a URL
    * or a file, and that doesn't apply here.) */
   if(IsFiltered())
      return false;

   /* If we're not line buffered, display only if the output CopyJob says to. */
   if(!output->GetCopy()->IsLineBuffered())
      return output->HasStatus();

   /* We're line buffered, so we can output a status line without stomping
    * on a partially output line.
    *
    * If we've output something recently, only send the output to the title,
    * to avoid flickering status for no reason.
    */
   if(!update_timer.Stopped()) {
      s->NextUpdateTitleOnly();
      return true;
   }

   /* If we're not reenabling the status bar, and the statusbar has
    * been turned off (due to output being reenabled), only send to
    * the title. */
   if(!statusbar_redisplay && output->GetCopy()->WriteAllowed())
   {
      s->NextUpdateTitleOnly();
      return true;
   }

   /* There hasn't been output in a while.  Stop the output again,
    * so the FileCopy will clear the StatusLine when there's more data. */
   output->GetCopy()->AllowWrite(false);

   return true;
}

/* Get our contribution to the status line, which is just the output
 * status, if any.  Input status is the job of the user object. */
const char *OutputJob::Status(const StatusLine *s)
{
   if(no_status)
      return "";

   /* Never show anything if we havn't even received any data yet; it won't
    * start connecting until then, so it's not interesting. */
   if(!initialized)
      return "";

   /* Use the status from the output CopyJob.  It'll be the one that's connecting
    * to a host, if applicable. */
   return output->Status(s,true);
}

void OutputJob::PutEOF()
{
   /* Make sure we've sent at least one (empty) block.  This ensures
    * that we always start the input->output code path. */
   Put("", 0);

   /* Send an EOF to the input peer; it'll send an EOF to the output peer
    * when all of its data is actually sent. */
   if(InputPeer())
      InputPeer()->PutEOF();
}

/* add a filter to the beginning of the list */
void OutputJob::PreFilter(const char *newfilter)
{
   if(!filter)
   {
      SetFilter(newfilter);
      return;
   }

   char *newstr = xasprintf("%s | %s", newfilter, filter);
   SetFilter(newstr);
   printf("new: '%s'\n", newstr);
   xfree(newstr);
}

void OutputJob::SetFilter(const char *newfilter)
{
   xfree(filter);
   filter=xstrdup(newfilter);
}

/* Return the width of the output.  If there's a filter, we can either
 * return -1 (we might be piping through "sed", changing the width),
 * or the width we know (which is sane for most pagers.)  I'm not sure
 * which is better. */
int OutputJob::GetWidth() const
{
   if(IsFiltered())
      return -1;
   return width;
}

/* Return true if the output is going directly to a TTY. */
bool OutputJob::IsTTY() const
{
   if(IsFiltered())
      return false;
   return is_a_tty;
}

/* Get the input FileCopyPeer; this is the buffer we write to. */
FileCopyPeer *OutputJob::InputPeer() const
{
   return input? input->GetGet():0;
}

/* Get the output FileCopyPeer (the FileCopyPeer that's doing the final output). */
FileCopyPeer *OutputJob::OutputPeer() const
{
   return output? output->GetPut():0;
}

/* We're done if the output is finished, or on error. */
int OutputJob::Done()
{
   if(Error())
      return true;
   
   if(!initialized)
      return false;

   if(input && !input->Done())
     return false;
   if(output && !output->Done())
     return false;
   
   return true;
}

int OutputJob::Do()
{
   return STALL;
}

/* Don't register errors until they're actually printed by
 * the sub-job (ie. it's also Done()). */
bool OutputJob::Error()
{
   if(error)
      return true;
   if(input && input->Error() && input->Done())
      error=true;
   if(output && input != output && output->Error() && output->Done())
      error=true;
   return error;
}

void OutputJob::Fg()
{
   super::Fg();
   if(input)
      input->Fg();
   if(output && input != output)
      output->Fg();
}

void OutputJob::Bg()
{
   if(output && input != output)
      output->Bg();
   if(input)
      input->Bg();
   super::Bg();
}

void OutputJob::Suspend()
{
   if(input)
      input->Suspend();
   if(output && input != output)
      output->Suspend();
   super::Suspend();
}

void OutputJob::Resume()
{
   if(input)
      input->Resume();
   if(output && input != output)
      output->Resume();
   super::Resume();
}

bool OutputJob::Full()
{
   if(input == 0)
      return false;

   /* It'd be nicer to just check copy->GetGet()->IsSuspended(), since
    * the FileCopy will suspend the Get end if the Put end gets filled.
    * However, it won't do that until it actually tries to send something. */
   int size = 0;
   if(input->GetPut())
      size += input->GetPut()->Buffered();
   if(input->GetGet())
      size += input->GetGet()->Buffered();
   if(input != output)
   {
      if(output->GetPut())
	 size += output->GetPut()->Buffered();
      if(output->GetGet())
	 size += output->GetGet()->Buffered();
   }

   return size >= 0x10000;
}

/* We'll actually go over the buffer limit here; that's OK; it's not a
 * strict value.  (It's not convenient to prevent that completely with
 * Format(), either.) */
void OutputJob::Put(const char *buf,int size)
{
   InitCopy();
   if(!InputPeer())
      return;

   update_timer.SetResource("cmd:status-interval",0);

   int oldpos = InputPeer()->GetPos();
   InputPeer()->Put(buf, size);
   InputPeer()->SetPos(oldpos);
}

void OutputJob::Format(const char *f,...)
{
   InitCopy();
   if(!InputPeer())
      return;

   update_timer.SetResource("cmd:status-interval",0);

   int oldpos = InputPeer()->GetPos();

   va_list v;
   va_start(v,f);
   InputPeer()->vFormat(f, v);
   va_end(v);

   InputPeer()->SetPos(oldpos);
}

/* Propagate signals down to our child processes. */
int OutputJob::AcceptSig(int sig)
{
   int m=MOVED;
   if(sig == SIGTERM || sig == SIGINT)
      m=WANTDIE;

   /* If we have an input copier right now, it'll contain the top filter
    * (which is linked to all other filters), so send it the signal. */
   if(input)
      input->AcceptSig(sig);
   /* Otherwise, the only filters we have running are in output_fd. */
   else if(output_fd)
      output_fd->Kill(sig);
   if(sig!=SIGCONT)
      AcceptSig(SIGCONT);
   return m;
}
