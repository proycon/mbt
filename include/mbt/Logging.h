/*
  $Id$
  $URL$

  Copyright (c) 1998 - 2012
  ILK   - Tilburg University
  CLiPS - University of Antwerp
 
  This file is part of mbt

  mbt is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  mbt is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.

  For questions and suggestions, see:
      http://ilk.uvt.nl/software.html
  or send mail to:
      timbl@uvt.nl
*/

#ifndef LOGGING_H
#define LOGGING_H

#include "ticcutils/LogStream.h"

extern LogStream default_log;
extern LogStream *cur_log;

#define LOG (*Log(cur_log))
#define DBG (*Dbg(cur_log))
#define xDBG (*xDbg(cur_log))
#define xxDBG (*xxDbg(cur_log))

extern LogLevel Tagger_Log_Level;

#endif // LOGGING_H
