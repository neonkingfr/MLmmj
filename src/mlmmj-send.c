/* Copyright (C) 2004, 2003, 2004 Mads Martin Joergensen <mmj at mmj.dk>
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
#include <limits.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include "mlmmj.h"
#include "mlmmj-send.h"
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
#include "statctrl.h"
#include "ctrlvalue.h"
#include "mylocking.h"
#include "getaddrsfromfd.h"

static int addtohdr = 0;
static int prepmailinmem = 0;
static int maxverprecips = MAXVERPRECIPS;
static int gotsigterm = 0;

void catch_sig_term(int sig)
{
	fprintf(stderr, "inside catch_sig_term");
	gotsigterm = 1;
}

char *bounce_from_adr(const char *recipient, const char *listadr,
		      const char *mailfilename)
{
	char *bounceaddr, *myrecipient, *mylistadr;
	char *indexstr, *listdomain, *a = NULL, *mymailfilename;
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
	if (a)
		*a = '=';

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
		 indexstr, myrecipient, listdomain);

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
			"-a", num,
			"-n", addr, (char *)NULL);

	log_error(LOG_ARGS, "execlp() of '%s' failed", mlmmjbounce);

	return 1;
}

int send_mail(int sockfd, const char *from, const char *to,
	      const char *replyto, char *mailmap, size_t mailsize,
	      const char *listdir, const char *mlmmjbounce,
	      const char *hdrs, size_t hdrslen, const char *body,
	      size_t bodylen)
{
	int retval = 0;
	char *reply, *tohdr;

	if(strchr(to, '@') == NULL) {
		errno = 0;
		log_error(LOG_ARGS, "No @ in address, ignoring %s",
				to);
		return 0;
	}
	
	retval = write_mail_from(sockfd, from, "");
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

	if(addtohdr)
		tohdr = concatstr(3, "To: ", to, "\r\n");
	else
		tohdr = NULL;

	if(prepmailinmem) {
		retval = writen(sockfd, hdrs, hdrslen);
		if(retval < 0) {
			log_error(LOG_ARGS, "Could not write mailheaders.\n");
			return retval;
		}
		if(tohdr) {
			retval = writen(sockfd, tohdr, strlen(tohdr));
			if(retval < 0) {
				log_error(LOG_ARGS, "Could not write To:.\n");
				return retval;
			}
			myfree(tohdr);
		}
		retval = writen(sockfd, body, bodylen);
		if(retval < 0) {
			log_error(LOG_ARGS, "Could not write mailbody.\n");
			return retval;
		}
	} else {
		retval = write_mailbody_from_map(sockfd, mailmap, mailsize,
						 tohdr);
		if(retval) {
			log_error(LOG_ARGS, "Could not write mail\n");
			return retval;
		}
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

int initsmtp(int *sockfd, const char *relayhost, unsigned short port)
{
	int retval = 0;
	char *reply = NULL;
	char *myhostname = hostnamestr();
	
	init_sockfd(sockfd, relayhost, port);
	
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
	char *reply = NULL;

	if(*sockfd == -1)
		return retval;
	
	write_quit(*sockfd);
	reply = checkwait_smtpreply(*sockfd, MLMMJ_QUIT);
	if(reply) {
		printf("reply from quit: %s\n", reply);
		log_error(LOG_ARGS, "Mailserver would not let us QUIT. "
			  "We close the socket anyway though. "
			  "Mailserver reply = [%s]", reply);
		myfree(reply);
		retval = MLMMJ_QUIT;
	}

	close(*sockfd);

	*sockfd = -1;

	return retval;
}

int send_mail_verp(int sockfd, struct strlist *addrs, char *mailmap,
		   size_t mailsize, const char *from, const char *listdir,
		   const char *hdrs, size_t hdrslen, const char *body,
		   size_t bodylen, const char *verpextra)
{
	int retval, i;
	char *reply;

	retval = write_mail_from(sockfd, from, verpextra);
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
	for(i = 0; i < addrs->count; i++) {
		if(gotsigterm) {
			log_error(LOG_ARGS, "TERM signal recieved, "
					"shutting down.");
			return -1;
		}
		if(strchr(addrs->strs[i], '@') == NULL) {
			errno = 0;
			log_error(LOG_ARGS, "No @ in address, ignoring %s",
					addrs->strs[i]);
			continue;
		}
		retval = write_rcpt_to(sockfd, addrs->strs[i]);
		if(retval) {
			log_error(LOG_ARGS, "Could not write RCPT TO:\n");
			return retval;
		}

		reply = checkwait_smtpreply(sockfd, MLMMJ_RCPTTO);
		if(reply) {
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

	if(prepmailinmem) {
		retval = writen(sockfd, hdrs, hdrslen);
		if(retval < 0) {
			log_error(LOG_ARGS, "Could not write mailheaders.\n");
			return retval;
		}
		retval = writen(sockfd, body, bodylen);
		if(retval < 0) {
			log_error(LOG_ARGS, "Could not write mailbody.\n");
			return retval;
		}
	} else {
		retval = write_mailbody_from_map(sockfd, mailmap, mailsize,
						 NULL);
		if(retval) {
			log_error(LOG_ARGS, "Could not write mail\n");
			return retval;
		}
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

int send_mail_many_fd(int sockfd, const char *from, const char *replyto,
		      char *mailmap, size_t mailsize, int subfd,
		      const char *listaddr, const char *archivefilename,
		      const char *listdir, const char *mlmmjbounce,
		      const char *hdrs, size_t hdrslen, const char *body,
		      size_t bodylen)
{
	int res, ret, i;
	struct strlist stl;

	stl.strs = NULL;
	stl.count = 0;

	do {
		res = getaddrsfromfd(&stl, subfd, maxverprecips);
		if(stl.count == maxverprecips) {
			ret = send_mail_many_list(sockfd, from, replyto,
					mailmap, mailsize, &stl, listaddr,
					archivefilename, listdir, mlmmjbounce,
					hdrs, hdrslen, body, bodylen);
			for(i = 0; i < stl.count; i++)
				myfree(stl.strs[i]);
			if(ret < 0)
				return ret;
			stl.count = 0;
		}
	} while(res > 0);

	if(stl.count) {
		ret = send_mail_many_list(sockfd, from, replyto, mailmap,
				mailsize, &stl, listaddr, archivefilename,
				listdir, mlmmjbounce, hdrs, hdrslen, body,
				bodylen);
		for(i = 0; i < stl.count; i++)
			myfree(stl.strs[i]);
		stl.count = 0;
		return ret;
	}

	return 0;
}

int requeuemail(const char *listdir, const char *index, struct strlist *addrs,
		int addrcount)
{
	int addrfd, i;
	char *dirname, *addrfilename, *addr;
	
	dirname = concatstr(3, listdir, "/requeue/", index);
	if(mkdir(dirname, 0750) < 0 && errno != EEXIST) {
		log_error(LOG_ARGS, "Could not mkdir(%s) for "
				"requeueing. Mail cannot "
				"be requeued.", dirname);
		myfree(dirname);
		return -1;
	}
	addrfilename = concatstr(2, dirname, "/subscribers");
	myfree(dirname);
	addrfd = open(addrfilename, O_WRONLY|O_CREAT|O_APPEND,
			S_IRUSR|S_IWUSR);
	if(addrfd < 0) {
		log_error(LOG_ARGS, "Could not open %s",
				addrfilename);
		myfree(addrfilename);
		return -1;
	} else {
		/* Dump the remaining addresses. We dump the remaining before
		 * we write the failing address to ensure the potential good
		 * ones will be tried first when mlmmj-maintd sends out mails
		 * that have been requeued. addrcount was so far we were */
		for(i = addrcount + 1; i < addrs->count; i++) {
			addr = concatstr(2, addrs->strs[i], "\n");
			if(writen(addrfd, addr, strlen(addr)) < 0) {
				log_error(LOG_ARGS, "Could not add [%s] "
						    "to requeue file", addr);
				return -1;
			}
			myfree(addr);
		}
		addr = concatstr(2, addrs->strs[addrcount], "\n");
		if(writen(addrfd, addr, strlen(addr)) < 0) {
			log_error(LOG_ARGS, "Could not add [%s] to requeue "
					"file", addr);
			return -1;
		}
		myfree(addr);
	}
	myfree(addrfilename);
	close(addrfd);

	return 0;
}

int send_mail_many_list(int sockfd, const char *from, const char *replyto,
		   char *mailmap, size_t mailsize, struct strlist *addrs,
		   const char *listaddr, const char *archivefilename,
		   const char *listdir, const char *mlmmjbounce,
		   const char *hdrs, size_t hdrslen, const char *body,
		   size_t bodylen)
{
	int res = 0, i;
	char *bounceaddr, *addr, *index;

	for(i = 0; i < addrs->count; i++) {
		addr = addrs->strs[i];
		if(strchr(addr, '@') == NULL) {
			errno = 0;
			log_error(LOG_ARGS, "No @ in address, ignoring %s",
					addr);
			continue;
		}
		if(gotsigterm && listaddr && archivefilename) {
			/* we got SIGTERM, so save the addresses and bail */
			log_error(LOG_ARGS, "TERM signal recieved, "
						"shutting down.");
			index = mybasename(archivefilename);
			return requeuemail(listdir, index, addrs, i);
		}
		if(from) {
			res = send_mail(sockfd, from, addr, replyto,
					    mailmap, mailsize, listdir, NULL,
					    hdrs, hdrslen, body, bodylen);
		} else {
			bounceaddr = bounce_from_adr(addr, listaddr,
						     archivefilename);
			res = send_mail(sockfd, bounceaddr, addr, replyto,
				  mailmap, mailsize, listdir, mlmmjbounce,
				  hdrs, hdrslen, body, bodylen);
			myfree(bounceaddr);
		}
		if(res && listaddr && archivefilename) {
			/* we failed, so save the addresses and bail */
			index = mybasename(archivefilename);
			return requeuemail(listdir, index, addrs, i);
		}
	}
	return 0;
}	

static void print_help(const char *prg)
{
        printf("Usage: %s [-L /path/to/list || -l listctrl] \n"
	       "       -m /path/to/mail [-a] [-D] [-F] [-h] [-r] [-R] "
	       "[-s] [-T] [-V]\n"
	       " -a: Don't archive the mail\n"
	       " -D: Don't delete the mail after it's sent\n"
	       " -F: What to use as MAIL FROM:\n"
	       " -h: This help\n"
	       " -l: List control variable:\n", prg);
	printf("    '1' means 'send a single mail'\n"
	       "    '2' means 'mail to moderators'\n"
	       "    '3' means 'resend failed list mail'\n"
	       "    '4' means 'send to file with recipients'\n"
	       "    '5' means 'bounceprobe'\n"
	       "    '6' means 'single listmail to single recipient'\n"
	       "    '7' means 'digest'\n"
	       " -L: Full path to list directory\n"
	       " -m: Full path to mail file\n"
	       " -r: Relayhost IP address (defaults to 127.0.0.1)\n"
	       " -R: What to use as Reply-To: header\n"
	       " -s: Subscribers file name\n"
	       " -T: What to use as RCPT TO:\n"
	       " -V: Print version\n");
	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	size_t len = 0, hdrslen, bodylen;
	int sockfd = -1, mailfd = 0, opt, mindex = 0, subfd = 0, tmpfd, i;
	int deletewhensent = 1, sendres = 0, archive = 1, digest = 0;
	int ctrlarchive, res;
	char *listaddr = NULL, *mailfilename = NULL, *subfilename = NULL;
	char *replyto = NULL, *bounceaddr = NULL, *to_addr = NULL;
	char *relayhost = NULL, *archivefilename = NULL, *tmpstr;
	char *listctrl = NULL, *subddirname = NULL, *listdir = NULL;
	char *mlmmjbounce = NULL, *bindir, *mailmap, *probefile, *a;
	char *body = NULL, *hdrs = NULL, *memmailsizestr = NULL, *verp = NULL;
	char relay[16], *listname, *listfqdn, *verpfrom, *maxverprecipsstr;
	char strindex[32], *reply, *strport;
	ssize_t memmailsize = 0;
	DIR *subddir;
	struct dirent *dp;
	struct stat st;
	struct hostent *relayent;
	uid_t uid;
	struct strlist stl;
	unsigned short smtpport = 25;
	struct sigaction sigact;

	CHECKFULLPATH(argv[0]);
	
	log_set_name(argv[0]);

	bindir = mydirname(argv[0]);
	mlmmjbounce = concatstr(2, bindir, "/mlmmj-bounce");
	myfree(bindir);
	
	/* install signal handler for SIGTERM */
	sigact.sa_handler = catch_sig_term;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	if(sigaction(SIGTERM, &sigact, NULL) < 0)
		log_error(LOG_ARGS, "Could not install SIGTERM handler!");

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

	/* Lets make sure no random user tries to send mail to the list */
	if(listdir) {
		if(stat(listdir, &st) == 0) {
			uid = getuid();
			if(uid && uid != st.st_uid) {
				log_error(LOG_ARGS,
					"Have to invoke either as root "
					"or as the user owning listdir");
				writen(STDERR_FILENO,
					"Have to invoke either as root "
					"or as the user owning listdir\n", 60);
				exit(EXIT_FAILURE);
			}
		} else {
			log_error(LOG_ARGS, "Could not stat %s", listdir);
			exit(EXIT_FAILURE);
		}
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

	if((listctrl[0] == '7' && listdir == NULL)) {
		fprintf(stderr, "With -l 7 you need -L\n");
		exit(EXIT_FAILURE);
	}

	verp = ctrlvalue(listdir, "verp");
	chomp(verp);
	if(verp == NULL)
		if(statctrl(listdir, "verp") == 1)
			verp = mystrdup("");

	maxverprecipsstr = ctrlvalue(listdir, "maxverprecips");
	if(verp && maxverprecipsstr) {
		maxverprecips = atol(maxverprecipsstr);
		myfree(maxverprecipsstr);
	}

	if(maxverprecips <= 0)
		maxverprecips = MAXVERPRECIPS;

	stl.strs = NULL;
	stl.count = 0;

	switch(listctrl[0]) {
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
			archive = 0;
		default:
			break;
	}

	if(listdir && listctrl[0] != '5')
		listaddr = getlistaddr(listdir);
	
	/* initialize file with mail to send */

	if((mailfd = open(mailfilename, O_RDWR)) < 0) {
	        log_error(LOG_ARGS, "Could not open '%s'", mailfilename);
		exit(EXIT_FAILURE);
	}

	if(myexcllock(mailfd) < 0) {
		log_error(LOG_ARGS, "Could not lock '%s'."
				"Mail not sent!", mailfilename);
		exit(EXIT_FAILURE);
	}

	if(fstat(mailfd, &st) < 0) {
		log_error(LOG_ARGS, "Could not stat mailfd");
		exit(EXIT_FAILURE);
	}

	addtohdr = statctrl(listdir, "addtohdr");
	memmailsizestr = ctrlvalue(listdir, "memorymailsize");
	ctrlarchive = statctrl(listdir, "noarchive");
	if(memmailsizestr) {
		memmailsize = strtol(memmailsizestr, NULL, 10);
		myfree(memmailsizestr);
	}

	if(memmailsize == 0)
		memmailsize = MEMORYMAILSIZE;

	if(st.st_size > memmailsize) {
		prepmailinmem = 0;
		errno = 0;
		log_error(LOG_ARGS, "Not preparing in memory. "
				    "Mail is %ld bytes", (long)st.st_size);
	} else
		prepmailinmem = 1;

	mailmap = mmap(0, st.st_size, PROT_READ, MAP_SHARED, mailfd, 0);
	if(mailmap == MAP_FAILED) {
		log_error(LOG_ARGS, "Could not mmap mailfd");
		exit(EXIT_FAILURE);
	}

	if(prepmailinmem) {
		hdrs = get_preppedhdrs_from_map(mailmap, &hdrslen);
		if(hdrs == NULL) {
			log_error(LOG_ARGS, "Could not prepare headers");
			exit(EXIT_FAILURE);
		}
		body = get_prepped_mailbody_from_map(mailmap, st.st_size,
						     &bodylen);
		if(body == NULL) {
			log_error(LOG_ARGS, "Could not prepare mailbody");
			myfree(hdrs);
			exit(EXIT_FAILURE);
		}
	}

	switch(listctrl[0]) {
	case '1': /* A single mail is to be sent, do nothing */
	case '5':
		break;
	case '2': /* Moderators */
		subfilename = concatstr(2, listdir, "/control/moderators");
		if((subfd = open(subfilename, O_RDONLY)) < 0) {
			log_error(LOG_ARGS, "Could not open '%s':",
					    subfilename);
			myfree(hdrs);
			myfree(body);
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
			myfree(hdrs);
			myfree(body);
			exit(EXIT_FAILURE);
		}
		break;
	case '6':
		archive = 0;
		deletewhensent = 0;
		archivefilename = mystrdup(mailfilename);
		bounceaddr = bounce_from_adr(to_addr, listaddr,
						archivefilename);
		break;
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

	itoa(mindex, strindex);

	if(!relayhost) {
		relayhost = ctrlvalue(listdir, "relayhost");
		chomp(relayhost);
	}
	if(!relayhost)
		strncpy(relay, RELAYHOST, sizeof(relay));
	else {
		relayent = gethostbyname(relayhost);
		if(relayent == NULL) {
			strncpy(relay, RELAYHOST, sizeof(relay));
		} else {
			if(inet_ntop(relayent->h_addrtype,
				     relayent->h_addr_list[0],
				     relay, sizeof(relay)) == NULL)
				strncpy(relay, RELAYHOST, sizeof(relay));
		}
	}
	strport = ctrlvalue(listdir, "smtpport");
	if(strport)
		smtpport = (unsigned short)atol(strport);

	switch(listctrl[0]) {
	case '1': /* A single mail is to be sent */
	case '6':
		initsmtp(&sockfd, relay, smtpport);
		sendres = send_mail(sockfd, bounceaddr, to_addr, replyto,
				mailmap, st.st_size, listdir, NULL,
				hdrs, hdrslen, body, bodylen);
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
		initsmtp(&sockfd, relay, smtpport);
		if(send_mail_many_fd(sockfd, bounceaddr, NULL, mailmap,
				     st.st_size, subfd, NULL, NULL, listdir,
				     NULL, hdrs, hdrslen, body, bodylen))
			close(sockfd);
		else
			endsmtp(&sockfd);
		break;
	case '3': /* resending earlier failed mails */
		initsmtp(&sockfd, relay, smtpport);
		if(send_mail_many_fd(sockfd, NULL, NULL, mailmap, st.st_size,
				subfd, listaddr, mailfilename, listdir,
				mlmmjbounce, hdrs, hdrslen, body, bodylen))
			close(sockfd);
		else
			endsmtp(&sockfd);
		unlink(subfilename);
		break;
	case '4': /* send mails to owner */
		initsmtp(&sockfd, relay, smtpport);
		if(send_mail_many_fd(sockfd, bounceaddr, NULL, mailmap,
				st.st_size, subfd, listaddr, mailfilename,
				listdir, mlmmjbounce, hdrs, hdrslen, body,
				bodylen))
			close(sockfd);
		else
			endsmtp(&sockfd);
		break;
	case '5': /* bounceprobe - handle relayhost local users bouncing*/
		initsmtp(&sockfd, relay, smtpport);
		sendres = send_mail(sockfd, bounceaddr, to_addr, replyto,
				mailmap, st.st_size, listdir, NULL,
				hdrs, hdrslen, body, bodylen);
		endsmtp(&sockfd);
		if(sendres) {
			/* error, so remove the probefile */
			tmpstr = mystrdup(to_addr);
			a = strchr(tmpstr, '@');
			*a = '=';
			probefile = concatstr(4, listdir, "/bounce/", tmpstr,
					"-probe");
			unlink(probefile);
			myfree(probefile);
			myfree(tmpstr);
		}
		break;
	case '7':
		digest = 1;
		addtohdr = 1;
		archivefilename = "digest";
		/* fall through */
	default: /* normal list mail */
		if (!digest) {
			subddirname = concatstr(2, listdir, "/subscribers.d/");
		} else {
			subddirname = concatstr(2, listdir, "/digesters.d/");
		}
		if((subddir = opendir(subddirname)) == NULL) {
			log_error(LOG_ARGS, "Could not opendir(%s)",
					    subddirname);
			myfree(subddirname);
			myfree(hdrs);
			myfree(body);
			exit(EXIT_FAILURE);
		}

		listname = genlistname(listaddr);	
		listfqdn = genlistfqdn(listaddr);	
		verpfrom = concatstr(5, listname, "+bounces-", strindex, "@",
				listfqdn);
		myfree(listname);
		myfree(listfqdn);

		if(digest)
			verp = NULL;

		if(verp && (strcmp(verp, "postfix") == 0)) {
			myfree(verp);
			verp = mystrdup("XVERP=-=");
		}

		if(addtohdr && verp) {
			log_error(LOG_ARGS, "Cannot use VERP and add "
					"To: header. Not sending with "
					"VERP.");
			verp = NULL;
		}
		
		if(verp) {
			initsmtp(&sockfd, relay, smtpport);
			if(write_mail_from(sockfd, verpfrom, verp)) {
				log_error(LOG_ARGS,
						"Could not write MAIL FROM\n");
				verp = NULL;
			} else {
				reply = checkwait_smtpreply(sockfd, MLMMJ_FROM);
				if(reply) {
					log_error(LOG_ARGS,
						"Mailserver did not "
						"accept verp mail from. "
						"Not sending with VERP.");
					myfree(reply);
					verp = NULL;
				}
			}
			endsmtp(&sockfd);
		}

		while((dp = readdir(subddir)) != NULL) {
			if(!strcmp(dp->d_name, "."))
				continue;
			if(!strcmp(dp->d_name, ".."))
				continue;
			subfilename = concatstr(2, subddirname, dp->d_name);
			if((subfd = open(subfilename, O_RDONLY)) < 0) {
				log_error(LOG_ARGS, "Could not open '%s'",
						    subfilename);
				myfree(subfilename);
				continue;
			}
			do {
				res = getaddrsfromfd(&stl, subfd,
						maxverprecips);
				if(stl.count == maxverprecips) {
					initsmtp(&sockfd, relay, smtpport);
					if(verp) {
						sendres = send_mail_verp(
								sockfd, &stl,
								mailmap,
								st.st_size,
								verpfrom,
								listdir, hdrs,
								hdrslen, body,
								bodylen, verp);
						if(sendres)
							requeuemail(listdir,
								strindex,
								&stl, 0);
					} else {
						sendres = send_mail_many_list(
								sockfd, NULL,
								NULL, mailmap,
								st.st_size,
								&stl,
								listaddr,
								archivefilename,
								listdir,
								mlmmjbounce,
								hdrs, hdrslen,
								body, bodylen);
					}
					endsmtp(&sockfd);
					for(i = 0; i < stl.count; i++)
						myfree(stl.strs[i]);
					stl.count = 0;
				}
			} while(res > 0);
			myfree(subfilename);
			close(subfd);

		}
		if(stl.count) {
			initsmtp(&sockfd, relay, smtpport);
			if(verp) {
				sendres = send_mail_verp(sockfd, &stl, mailmap,
						st.st_size, verpfrom, listdir,
						hdrs, hdrslen, body, bodylen,
						verp);
				if(sendres)
					requeuemail(listdir, strindex, &stl,
							0);
			} else {
				sendres = send_mail_many_list(sockfd, NULL,
						NULL, mailmap, st.st_size,
						&stl, listaddr,
						archivefilename, listdir,
						mlmmjbounce, hdrs, hdrslen,
						body, bodylen);
			}
			endsmtp(&sockfd);
			for(i = 0; i < stl.count; i++)
				myfree(stl.strs[i]);
			stl.count = 0;
		}

		if (sendres) {
			/* If send_mail_many() failed we close the
			 * connection to the mail server in a brutal
			 * manner, because we could be in any state
			 * (DATA for instance). */
			close(sockfd);
		} else {
			endsmtp(&sockfd);
		}
		myfree(stl.strs);
		myfree(verpfrom);
		closedir(subddir);
		myfree(subddirname);
		break;
	}
	
	for(i = 0; i < stl.count; i++)
		myfree(stl.strs[i]);
	stl.count = 0;

	myfree(hdrs);
	myfree(body);
	myfree(mlmmjbounce);
	close(sockfd);
	munmap(mailmap, st.st_size);
	close(mailfd);
	myfree(verp);

	if(archive) {
		if(!ctrlarchive) {
			rename(mailfilename, archivefilename);
		} else {
			unlink(mailfilename);
		}
		myfree(archivefilename);
	} else if(deletewhensent)
		unlink(mailfilename);

	return EXIT_SUCCESS;
}
