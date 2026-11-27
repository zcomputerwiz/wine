;
; WineExplorer: messages of the sources the EventLog service and the journal bridge write
; (docs/EVENTLOG_DESIGN.md section 9, docs/EVENTLOG_JOURNAL.md). Our own wording: the event ids and the order of the
; insertion strings are the documented ones, the text is not taken from Windows.
;
; Copyright 2026 WineExplorer Desktop Environment Project
;
; This library is free software; you can redistribute it and/or
; modify it under the terms of the GNU Lesser General Public
; License as published by the Free Software Foundation; either
; version 2.1 of the License, or (at your option) any later version.
;
; This library is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
; Lesser General Public License for more details.
;
; You should have received a copy of the GNU Lesser General Public
; License along with this library; if not, write to the Free Software
; Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
;

LanguageNames=(ENU=0x409:wevtmsg)

; Linux Kernel, and the sources of listed units: the journal entry's own text
MessageId=1
SymbolicName=WEX_MSG_TEXT
Language=ENU
%1
.
MessageId=2
SymbolicName=WEX_MSG_FS_ERROR
Language=ENU
%1
.
MessageId=3
SymbolicName=WEX_MSG_OOM
Language=ENU
%1
.
MessageId=4
SymbolicName=WEX_MSG_COREDUMP
Language=ENU
The program %1 ended with a core dump (signal %2).
.
MessageId=5
SymbolicName=WEX_MSG_RATE_LIMITED
Language=ENU
%3 journal entries for rule %1 (source %2) were not stored in the last minute: they came faster than the rate limit allows.
.
MessageId=6
SymbolicName=WEX_MSG_JOURNAL_GAP
Language=ENU
Journal entries were lost between the last one stored here and the oldest one the journal still keeps.
.
; Disk
MessageId=11
SymbolicName=WEX_MSG_DISK_ERROR
Language=ENU
The device %1 reported an input/output error.
.
; EventLog
MessageId=6000
SymbolicName=WEX_MSG_LOG_FULL
Language=ENU
The %1 log is full, and no further events can be stored in it.
.
; Service Control Manager
MessageId=7000
SymbolicName=WEX_MSG_SERVICE_START_FAILED
Language=ENU
The %1 service could not be started: %2
.
MessageId=7031
SymbolicName=WEX_MSG_SERVICE_CRASH_RESTART
Language=ENU
The %1 service stopped unexpectedly, %2 time(s) so far. In %3 milliseconds this will be done: %4.
.
MessageId=7034
SymbolicName=WEX_MSG_SERVICE_CRASH
Language=ENU
The %1 service stopped unexpectedly, %2 time(s) so far.
.
