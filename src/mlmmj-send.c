/* Copyright (C) 2002, 2003, 2004 Mads Martin Joergensen <mmj at mmj.dk>
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/wait.h>
#include <signal.h>
#include <libgen.h>
#include <syslog.h>
#include <stdarg.h>
#include <sys/mman.h>

#include "mlmmj-send.h"
#include "mlmmj.h"
#include "mail-functions.h"
#include "itoa.h"
#include "incindexfile.h"
#include "chomp.h"
#include "checkwait_smtpreply.h"
#include "getlistaddr.h"
#include "init_sockfd.h"
#include "strgen.h"
#include "log_error.h"
#include "mygetline.h"
#include "wrappers.h"
#include "memory.h"

char *bounce_from_adr(const char *recipient, const char *listadr,
		      const char *mailfilename)
{
	char *bounceaddr, *myrecipient, *mylistadr;
	char *indexstr, *listdomain, *a, *mymailfilename;
	size_t len;

	mymailfilename = mystrdup(mailfilename);
	if (!mymailfilename) {
		return NULL;
	}

	indexstr = strrchr(mymailfilename, '/');
	if (indexstr) {
		indexstr++;  /* skip the slash */
	} else {
		indexstr = mymailfilename;
	}

	myrecipient = mystrdup(recipient);
	if (!myrecipient) {
		myfree(mymailfilename);
		return NULL;
	}
	a = strchr(myrecipient, '@');
	if (a) *a = '=';

	mylistadr = mystrdup(listadr);
	if (!mylistadr) {
		myfree(mymailfilename);
		myfree(myrecipient);
		return NULL;
	}

	listdomain = strchr(mylistadr, '@');
	if (!listdomain) {
		myfree(mymailfilename);
		myfree(myrecipient);
		myfree(mylistadr);
		return NULL;
	}
	*listdomain++ = '\0';

	/* 12 = RECIPDELIM + "bounces-" + "-" + "@" + NUL */
	len = strlen(mylistadr) + strlen(myrecipient) + strlen(indexstr)
		 + strlen(listdomain) + 12;
	bounceaddr = mymalloc(len);
	if (!bounceaddr) {
		myfree(myrecipient);
		myfree(mylistadr);
		return NULL;
	}
	snprintf(bounceaddr, len, "%s%cbounces-%s-%s@%s", mylistadr, RECIPDELIM,
		 myrecipient, indexstr, listdomain);

	myfree(myrecipient);
	myfree(mylistadr);
	myfree(mymailfilename);

	return bounceaddr;
}

int bouncemail(const char *listdir, const char *mlmmjbounce, const char *from)
{
	char *myfrom = mystrdup(from);
	char *addr, *num, *c;
	size_t len;
	pid_t pid = 0;

	if((c = strchr(myfrom, '@')) == NULL) {
		myfree(myfrom);
		return 0; /* Success when malformed 'from' */
	}
	*c = '\0';
	num = strrchr(myfrom, '-');
	num++;
	c = strchr(myfrom, RECIPDELIM);
	myfrom = strchr(c, '-');
	myfrom++;
	len = num - myfrom - 1;
	addr = mymalloc(len + 1);
	addr[len] = '\0';
	strncpy(addr, myfrom, len);

	pid = fork();
	
	if(pid < 0) {
		log_error(LOG_ARGS, "fork() failed!");
		return 1;
	}
	
	if(pid > 0)
		return 0;
	
	execlp(mlmmjbounce, mlmmjbounce,
			"-L", listdir,
			"-a", addr,
			"-n", num, 0);

	log_error(LOG_ARGS, "execlp() of '%s' failed", mlmmjbounce);

	return 1;
}

int send_mail(int sockfd, const char *from, const char *to,
	      const char *replyto, char *mailmap, size_t mailsize,
	      const char *listdir, const char *mlmmjbounce)
{
	int retval = 0;
	char *reply;
	
	retval = write_mail_from(sockfd, from);
	if(retval) {
		log_error(LOG_ARGS, "Could not write MAIL FROM\n");
		return retval;
	}
	reply = checkwait_smtpreply(sockfd, MLMMJ_FROM);
	if(reply) {
		log_error(LOG_ARGS, "Error in MAIL FROM. Reply = [%s]",
				reply);
		myfree(reply);
		write_rset(sockfd);
		checkwait_smtpreply(sockfd, MLMMJ_RSET);
		return MLMMJ_FROM;
	}
	retval = write_rcpt_to(sockfd, to);
	if(retval) {
		log_error(LOG_ARGS, "Could not write RCPT TO:\n");
		return retval;
	}

	reply = checkwait_smtpreply(sockfd, MLMMJ_RCPTTO);
	if(reply) {
		write_rset(sockfd);
		checkwait_smtpreply(sockfd, MLMMJ_RSET);
		if(mlmmjbounce && ((reply[0] == '4') || (reply[0] == '5'))
				&& (reply[1] == '5')) {
			myfree(reply);
			return bouncemail(listdir, mlmmjbounce, from);
		} else {
			log_error(LOG_ARGS, "Error in RCPT TO. Reply = [%s]",
					reply);
			myfree(reply);
			return MLMMJ_RCPTTO;
		}
	}

	retval = write_data(sockfd);
	if(retval) {
		log_error(LOG_ARGS, "Could not write DATA\b");
		return retval;
	}

	reply = checkwait_smtpreply(sockfd, MLMMJ_DATA);
	if(reply) {
		log_error(LOG_ARGS, "Error with DATA. Reply = [%s]", reply);
		myfree(reply);
		write_rset(sockfd);
		checkwait_smtpreply(sockfd, MLMMJ_RSET);
		return MLMMJ_DATA;
	}

	if(replyto) {
		retval = write_replyto(sockfd, replyto);
		if(retval) {
			log_error(LOG_ARGS, "Could not write reply-to addr.\n");
			return retval;
		}
	}

	retval = write_mailbody_from_map(sockfd, mailmap, mailsize);
	if(retval) {
		log_error(LOG_ARGS, "Could not write mailbody\n");
		return retval;
	}

	retval = write_dot(sockfd);
	if(retval) {
		log_error(LOG_ARGS, "Could not write <CR><LF>.<CR><LF>\n");
		return retval;
	}

	reply = checkwait_smtpreply(sockfd, MLMMJ_DOT);
	if(reply) {
		log_error(LOG_ARGS, "Mailserver did not ack end of mail.\n"
				"<CR><LF>.<CR><LF> was written, to no"
				"avail. Reply = [%s]", reply);
		myfree(reply);
		write_rset(sockfd);
		checkwait_smtpreply(sockfd, MLMMJ_RSET);
		return MLMMJ_DOT;
	}

	return 0;
}

int initsmtp(int *sockfd, const char *relayhost)
{
	int retval = 0;
	char *reply = NULL;
	char *myhostname = hostnamestr();
	
	init_sockfd(sockfd, relayhost);
	
	if((reply = checkwait_smtpreply(*sockfd, MLMMJ_CONNECT)) != NULL) {
		log_error(LOG_ARGS, "No proper greeting to our connect"
			  "Reply: [%s]", reply);
		myfree(reply);
		retval = MLMMJ_CONNECT;
		/* FIXME: Queue etc. */
	}	
	write_helo(*sockfd, myhostname);
	myfree(myhostname);
	if((reply = checkwait_smtpreply(*sockfd, MLMMJ_HELO)) != NULL) {
		log_error(LOG_ARGS, "Error with HELO. Reply: [%s]", reply);
		/* FIXME: quit and tell admin to configure correctly */
		myfree(reply);
		retval = MLMMJ_HELO;
	}

	return retval;
}

int endsmtp(int *sockfd)
{
	int retval = 0;
	char *reply;
	
	write_quit(*sockfd);

	if((reply = checkwait_smtpreply(*sockfd, MLMMJ_QUIT)) != 0) {
		log_error(LOG_ARGS, "Mailserver would not let us QUIT. "
			  "We close the socket anyway though.");
		myfree(reply);
		retval = MLMMJ_QUIT;
	}

	close(*sockfd);

	return retval;
}

int send_mail_many(int sockfd, const char *from, const char *replyto,
		   char *mailmap, size_t mailsize, int subfd,
		   const char *listaddr, const char *archivefilename,
		   const char *listdir, const char *mlmmjbounce)
{
	int sendres = 0, addrfd;
	char *bounceaddr, *addr, *index, *dirname, *addrfilename;
	char *start, *cur, *next;
	struct stat st;
	size_t len;

	if(fstat(subfd, &st) < 0) {
		log_error(LOG_ARGS, "Could not stat subfd");
		return -1;
	}

	start = mmap(0, st.st_size, PROT_READ, MAP_SHARED, subfd, 0);
	if(start == MAP_FAILED) {
		log_error(LOG_ARGS, "Could not mmap subfd");
		return -1;
	}

	for(next = cur = start; next < start + st.st_size; next++) {
		if(*next == '\n' || next == start + st.st_size - 1) {
			len = next - cur;
			if(next == start + st.st_size - 1 && *next != '\n')
				len++;
			addr = mymalloc(len + 1);
			strncpy(addr, cur, len);
			addr[len] = '\0';
			cur = next + 1;
		} else
			continue;

		if(from)
			sendres = send_mail(sockfd, from, addr, replyto,
					    mailmap, mailsize, listdir, NULL);
		else {
			bounceaddr = bounce_from_adr(addr, listaddr,
						     archivefilename);
			sendres = send_mail(sockfd, bounceaddr, addr, replyto,
				  mailmap, mailsize, listdir, mlmmjbounce);
			myfree(bounceaddr);
		}
		if(sendres && listaddr && archivefilename) {
			/* we failed, so save the addresses and bail */
			index = mybasename(archivefilename);	
			dirname = concatstr(3, listdir, "/requeue/", index);
			myfree(index);
			if(mkdir(dirname, 0750) < 0) {
				log_error(LOG_ARGS, "Could not mkdir(%s) for "
						    "requeueing. Mail cannot "
						    "be requeued.", dirname);
				myfree(dirname);
				myfree(addr);
				return -1;
			}
			addrfilename = concatstr(2, dirname, "/subscribers");
			myfree(dirname);
			addrfd = open(addrfilename, O_WRONLY|O_CREAT|O_APPEND,
							S_IRUSR|S_IWUSR);
			if(addrfd < 0) {
				log_error(LOG_ARGS, "Could not write to %s",
						    addrfilename);
				myfree(addrfilename);
				myfree(addr);
				return -1;
			} else { /* dump the remaining addresses */
				do {
					/* Dirty hack to add newline. */
					len = strlen(addr);
					if(writen(addrfd, addr, len) < 0)
						log_error(LOG_ARGS,
							"Could not add [%s] "
							"to requeue address "
							"file.", addr);
					myfree(addr);
					addr = mygetline(subfd);
				} while(addr);
			}
			
			myfree(addr);
			myfree(addrfilename);
			close(addrfd);

			return -1;
		}
		myfree(addr);
	}
	return 0;
}	

static void print_help(const char *prg)
{
        printf("Usage: %s [-L /path/to/list || -l listctrl] -m /path/to/mail "
	       "[-a] [-D] [-F]\n"
	       "       [-h] [-r] [-R] [-s] [-T] [-V]\n"
	       " -a: Don't archive the mail\n"
	       " -D: Don't delete the mail after it's sent\n"
	       " -F: What to use as MAIL FROM:\n"
	       " -h: This help\n"
	       " -l: List control variable:\n", prg);
	printf("    '1' means 'send a single mail'\n"
	       "    '2' means 'mail to moderators'\n"
	       "    '3' means 'resend failed list mail'\n"
	       " -L: Full path to list directory\n"
	       " -m: Full path to mail file\n"
	       " -r: Relayhost (defaults to localhost)\n"
	       " -R: What to use as Reply-To: header\n"
	       " -s: Subscribers file name\n"
	       " -T: What to use as RCPT TO:\n"
	       " -V: Print version\n");
	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	size_t len = 0;
	int sockfd = 0, mailfd = 0, opt, mindex, subfd, tmpfd;
	int deletewhensent = 1, sendres, archive = 1;
	char *listaddr, *mailfilename = NULL, *subfilename = NULL;
	char *replyto = NULL, *bounceaddr = NULL, *to_addr = NULL;
	char *relayhost = NULL, *archivefilename = NULL, *tmpstr;
	char *listctrl = NULL, *subddirname = NULL, *listdir = NULL;
	char *mlmmjbounce = NULL, *bindir, *mailmap;
	DIR *subddir;
	struct dirent *dp;
	struct stat st;

	CHECKFULLPATH(argv[0]);
	
	log_set_name(argv[0]);

	bindir = mydirname(argv[0]);
	mlmmjbounce = concatstr(2, bindir, "/mlmmj-bounce");
	myfree(bindir);
	
	while ((opt = getopt(argc, argv, "aVDhm:l:L:R:F:T:r:s:")) != -1){
		switch(opt) {
		case 'a':
			archive = 0;
			break;
		case 'D':
			deletewhensent = 0;
			break;
		case 'F':
			bounceaddr = optarg;
			break;
		case 'h':
			print_help(argv[0]);
			break;
		case 'l':
			listctrl = optarg;
			break;
		case 'L':
			listdir = optarg;
			break;
		case 'm':
			mailfilename = optarg;
			break;
		case 'r':
			relayhost = optarg;
			break;
		case 'R':
			replyto = optarg;
			break;
		case 's':
			subfilename = optarg;
			break;
		case 'T':
			to_addr = optarg;
			break;
		case 'V':
			print_version(argv[0]);
			exit(EXIT_SUCCESS);
		}
	}

	if(mailfilename == NULL || (listdir == NULL && listctrl == NULL)) {
		fprintf(stderr, "You have to specify -m and -L or -l\n");
		fprintf(stderr, "%s -h for help\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if(!listctrl)
		listctrl = mystrdup("0");

	
	/* get the list address */
	if(listctrl[0] == '1' && (bounceaddr == NULL || to_addr == NULL)) {
		fprintf(stderr, "With -l 1 you need -F and -T\n");
		exit(EXIT_FAILURE);
	}

	if((listctrl[0] == '2' && (listdir == NULL || bounceaddr == NULL))) {
		fprintf(stderr, "With -l 2 you need -L and -F\n");
		exit(EXIT_FAILURE);
	}

	if(listctrl[0] == '1' || listctrl[0] == '2' || listctrl[0] == '3')
		archive = 0;

	if(listdir)
		listaddr = getlistaddr(listdir);
	
	/* initialize file with mail to send */

	if((mailfd = open(mailfilename, O_RDONLY)) < 0) {
	        log_error(LOG_ARGS, "Could not open '%s'", mailfilename);
		exit(EXIT_FAILURE);
	}

	if(fstat(mailfd, &st) < 0) {
		log_error(LOG_ARGS, "Could not stat mailfd");
		exit(EXIT_FAILURE);
	}

	mailmap = mmap(0, st.st_size, PROT_READ, MAP_SHARED, mailfd, 0);
	if(mailmap == MAP_FAILED) {
		log_error(LOG_ARGS, "Could not mmap mailfd");
		exit(EXIT_FAILURE);
	}

	switch(listctrl[0]) {
	case '1': /* A single mail is to be sent, do nothing */
		break;
	case '2': /* Moderators */
		subfilename = concatstr(2, listdir, "/control/moderators");
		if((subfd = open(subfilename, O_RDONLY)) < 0) {
			log_error(LOG_ARGS, "Could not open '%s':",
					    subfilename);
			myfree(subfilename);
			/* No moderators is no error. Could be the sysadmin
			 * likes to do it manually.
			 */
			exit(EXIT_SUCCESS);
		}
		break;
	case '3':
	case '4': /* sending mails to subfile */
		if((subfd = open(subfilename, O_RDONLY)) < 0) {
			log_error(LOG_ARGS, "Could not open '%s':",
					    subfilename);
			exit(EXIT_FAILURE);
		}

	default: /* normal list mail -- now handled when forking */
		break;
	}

	/* initialize the archive filename */
	if(archive) {
		mindex = incindexfile((const char *)listdir);
		len = strlen(listdir) + 9 + 20;
		archivefilename = mymalloc(len);
		snprintf(archivefilename, len, "%s/archive/%d", listdir,
			 mindex);
	}

	if(!relayhost)
		relayhost = mystrdup(RELAYHOST);

	switch(listctrl[0]) {
	case '1': /* A single mail is to be sent */
		initsmtp(&sockfd, relayhost);
		sendres = send_mail(sockfd, bounceaddr, to_addr, replyto,
				mailmap, st.st_size, listdir, NULL);
		endsmtp(&sockfd);
		if(sendres) {
			/* error, so keep it in the queue */
			deletewhensent = 0;
			/* dump date we want when resending */
			tmpstr = concatstr(2, mailfilename, ".mailfrom");
			tmpfd = open(tmpstr, O_WRONLY|O_CREAT|O_TRUNC,
						S_IRUSR|S_IWUSR);
			myfree(tmpstr);
			if(tmpfd >= 0) {
				writen(tmpfd, bounceaddr, strlen(bounceaddr));
				fsync(tmpfd);
			}
			close(tmpfd);
			tmpstr = concatstr(2, mailfilename, ".reciptto");
			tmpfd = open(tmpstr, O_WRONLY|O_CREAT|O_TRUNC,
						S_IRUSR|S_IWUSR);
			myfree(tmpstr);
			if(tmpfd >= 0) {
				writen(tmpfd, to_addr, strlen(to_addr));
				fsync(tmpfd);
			}
			close(tmpfd);
			if(replyto) {
				tmpstr = concatstr(2, mailfilename,
						      ".reply-to");
				tmpfd = open(tmpstr, O_WRONLY|O_CREAT|O_TRUNC,
							S_IRUSR|S_IWUSR);
				myfree(tmpstr);
				if(tmpfd >= 0) {
					writen(tmpfd, replyto,
						strlen(replyto));
					fsync(tmpfd);
				}
				close(tmpfd);
			}
		}
		break;
	case '2': /* Moderators */
		initsmtp(&sockfd, relayhost);
		if(send_mail_many(sockfd, bounceaddr, NULL, mailmap, st.st_size,
				subfd, NULL, NULL, listdir, NULL))
			close(sockfd);
		else
			endsmtp(&sockfd);
		break;
	case '3': /* resending earlier failed mails */
		initsmtp(&sockfd, relayhost);
		if(send_mail_many(sockfd, NULL, NULL, mailmap, st.st_size,
				subfd, listaddr, mailfilename, listdir,
				mlmmjbounce))
			close(sockfd);
		else
			endsmtp(&sockfd);
		unlink(subfilename);
		break;
	case '4': /* send mails to owner */
		initsmtp(&sockfd, relayhost);
		if(send_mail_many(sockfd, bounceaddr, NULL, mailmap, st.st_size,
				subfd, listaddr, mailfilename, listdir,
				mlmmjbounce))
			close(sockfd);
		else
			endsmtp(&sockfd);
		break;
	default: /* normal list mail */
		subddirname = concatstr(2, listdir, "/subscribers.d/");
		if((subddir = opendir(subddirname)) == NULL) {
			log_error(LOG_ARGS, "Could not opendir(%s)",
					    subddirname);
			myfree(subddirname);
			exit(EXIT_FAILURE);
		}
		myfree(subddirname);

		while((dp = readdir(subddir)) != NULL) {
			if(!strcmp(dp->d_name, "."))
				continue;
			if(!strcmp(dp->d_name, ".."))
				continue;
			subfilename = concatstr(3, listdir, "/subscribers.d/",
						dp->d_name);
			if((subfd = open(subfilename, O_RDONLY)) < 0) {
				log_error(LOG_ARGS, "Could not open '%s'",
						    subfilename);
				myfree(subfilename);
				continue;
			}
			myfree(subfilename);

			initsmtp(&sockfd, relayhost);
			sendres = send_mail_many(sockfd, NULL, NULL, mailmap,
					st.st_size, subfd, listaddr,
					archivefilename, listdir, mlmmjbounce);
			if (sendres) {
				/* If send_mail_many() failed we close the
				 * connection to the mail server in a brutal
				 * manner, because we could be in any state
				 * (DATA for instance). */
				close(sockfd);
			} else {
				endsmtp(&sockfd);
			}
			close(subfd);
		}
		closedir(subddir);
		break;
	}
	
	munmap(mailmap, st.st_size);
	close(mailfd);
	
	if(archive) {
		rename(mailfilename, archivefilename);
		myfree(archivefilename);
	} else if(deletewhensent)
		unlink(mailfilename);

	return EXIT_SUCCESS;
}
