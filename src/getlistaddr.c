/* Copyright (C) 2002, 2003 Mads Martin Joergensen <mmj at mmj.dk>
 *
 * $Id$
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "getlistaddr.h"
#include "chomp.h"
#include "log_error.h"
#include "mygetline.h"
#include "strgen.h"

char *getlistaddr(const char *listdir)
{
	char *tmpstr;
	int listnamefd;

	tmpstr = concatstr(2, listdir, "/control/listaddress");;
	if((listnamefd = open(tmpstr, O_RDONLY)) < 0) {
		log_error(LOG_ARGS, "Could not open '%s'", tmpstr);
		exit(EXIT_FAILURE);
	}
	free(tmpstr);

	tmpstr = mygetline(listnamefd);

	if(tmpstr == NULL){
		log_error(LOG_ARGS, "FATAL. Could not get listaddress in %s",
					listdir);
		exit(EXIT_FAILURE);
	}

	chomp(tmpstr);
	close(listnamefd);

	return tmpstr;
}
