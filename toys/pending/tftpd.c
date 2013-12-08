/* tftpd.c - TFTP server.
 *
 * Copyright 2013 Ranjan Kumar <ranjankumar.bth@gmail.com>
 * Copyright 2013 Kyungwan Han <asura321@gmail.com>
 *
 * No Standard.

USE_TFTPD(NEWTOY(tftpd, "rcu:l", TOYFLAG_BIN))

config TFTPD
  bool "tftpd"
  default n
  help
    usage: tftpd [-cr] [-u USER] [DIR]

    Transfer file from/to tftp server.

    -r	Prohibit upload
    -c	Allow file creation via upload
    -u	Access files as USER
    -l	Log to syslog (inetd mode requires this)
*/
#define FOR_tftpd
#include "toys.h"

GLOBALS(
  char *user;
  long sfd;
  struct passwd *pw;
)

#define TFTPD_BLKSIZE 512  // as per RFC 1350.

// opcodes
#define TFTPD_OP_RRQ  1  // Read Request          RFC 1350, RFC 2090
#define TFTPD_OP_WRQ  2  // Write Request         RFC 1350
#define TFTPD_OP_DATA 3  // Data chunk            RFC 1350
#define TFTPD_OP_ACK  4  // Acknowledgement       RFC 1350
#define TFTPD_OP_ERR  5  // Error Message         RFC 1350
#define TFTPD_OP_OACK 6  // Option acknowledgment RFC 2347

// Error Codes:
#define TFTPD_ER_NOSUCHFILE  1 // File not found
#define TFTPD_ER_ACCESS      2 // Access violation
#define TFTPD_ER_FULL        3 // Disk full or allocation exceeded
#define TFTPD_ER_ILLEGALOP   4 // Illegal TFTP operation
#define TFTPD_ER_UNKID       5 // Unknown transfer ID
#define TFTPD_ER_EXISTS      6 // File already exists
#define TFTPD_ER_UNKUSER     7 // No such user
#define TFTPD_ER_NEGOTIATE   8 // Terminate transfer due to option negotiation

/* TFTP Packet Formats
 *  Type   Op #     Format without header
 *         2 bytes    string    1 byte    string    1 byte
 *         -----------------------------------------------
 *  RRQ/  | 01/02 |  Filename  |   0  |    Mode    |   0  |
 *  WRQ    -----------------------------------------------
 *         2 bytes    2 bytes      n bytes
 *         ---------------------------------
 *  DATA  | 03    |   Block #  |    Data    |
 *         ---------------------------------
 *         2 bytes    2 bytes
 *         -------------------
 *  ACK   | 04    |   Block #  |
 *         --------------------
 *         2 bytes  2 bytes       string     1 byte
 *         ----------------------------------------
 *  ERROR | 05    |  ErrorCode |   ErrMsg   |   0  |
 *         ----------------------------------------
 */

static char *g_errpkt = toybuf + TFTPD_BLKSIZE;

// Create and send error packet.
static void send_errpkt(struct sockaddr *dstaddr,
    socklen_t socklen, char *errmsg)
{
  error_msg(errmsg);
  g_errpkt[1] = TFTPD_OP_ERR;
  strcpy(g_errpkt + 4, errmsg);
  if (sendto(TT.sfd, g_errpkt, strlen(errmsg)+5, 0, dstaddr, socklen) < 0)
    perror_exit("sendto failed");
}

// Used to send / receive packets.
static void do_action(struct sockaddr *srcaddr, struct sockaddr *dstaddr,
    socklen_t socklen, char *file, int opcode, int tsize, int blksize)
{
  int fd, done = 0, retry_count = 12, timeout = 100, len;
  uint16_t blockno = 1, pktopcode, rblockno;
  char *ptr, *spkt, *rpkt;
  struct pollfd pollfds[1];

  spkt = xzalloc(blksize + 4);
  rpkt = xzalloc(blksize + 4);
  ptr = spkt+2; //point after opcode.

  pollfds[0].fd = TT.sfd;
  // initialize groups, setgid and setuid
  if (TT.pw) {
    if (change_identity(TT.pw)) perror_exit("Failed to change identity");
    endgrent();
  }

  if (opcode == TFTPD_OP_RRQ) fd = open(file, O_RDONLY, 0666);
  else fd = open(file, ((toys.optflags & FLAG_c) ?
        (O_WRONLY|O_TRUNC|O_CREAT) : (O_WRONLY|O_TRUNC)) , 0666);
  if (fd < 0) {
    g_errpkt[3] = TFTPD_ER_NOSUCHFILE;
    send_errpkt(dstaddr, socklen, "can't open file");
    goto CLEAN_APP;
  }
  // For download -> blockno will be 1.
  // 1st ACK will be from dst,which will have blockno-=1
  // Create and send ACK packet.
  if (blksize != TFTPD_BLKSIZE || tsize) {
    pktopcode = TFTPD_OP_OACK;
    // add "blksize\000blksize_val\000" in send buffer.
    if (blksize != TFTPD_BLKSIZE) {
      strcpy(ptr, "blksize");
      ptr += strlen("blksize") + 1;
      ptr += snprintf(ptr, 6, "%d", blksize) + 1;
    }
    if (tsize) {// add "tsize\000tsize_val\000" in send buffer.
      struct stat sb;

      sb.st_size = 0;
      fstat(fd, &sb);
      strcpy(ptr, "tsize");
      ptr += strlen("tsize") + 1;
      ptr += sprintf(ptr, "%lu", (unsigned long)sb.st_size)+1;
    }
    goto SEND_PKT;
  }
  // upload ->  ACK 1st packet with filename, as it has blockno 0.
  if (opcode == TFTPD_OP_WRQ) blockno = 0;

  // Prepare DATA and/or ACK pkt and send it.
  for (;;) {
    int poll_ret;

    retry_count = 12, timeout = 100, pktopcode = TFTPD_OP_ACK;
    ptr = spkt+2;
    *((uint16_t*)ptr) = htons(blockno);
    blockno++;
    ptr += 2;
    if (opcode == TFTPD_OP_RRQ) {
      pktopcode = TFTPD_OP_DATA;
      len = readall(fd, ptr, blksize);
      if (len < 0) {
        send_errpkt(dstaddr, socklen, "read-error");
        break;
      }
      if (len != blksize) done = 1; //last pkt.
      ptr += len;
    }
SEND_PKT:
    // 1st ACK will be from dst, which will have blockno-=1
    *((uint16_t*)spkt) = htons(pktopcode); //append send pkt's opcode.
RETRY_SEND:
    if (sendto(TT.sfd, spkt, (ptr - spkt), 0, dstaddr, socklen) <0)
      perror_exit("sendto failed");
    // if "block size < 512", send ACK and exit.
    if ((pktopcode == TFTPD_OP_ACK) && done) break;

POLL_IN:
    pollfds[0].events = POLLIN;
    pollfds[0].fd = TT.sfd;
    poll_ret = poll(pollfds, 1, timeout);
    if (poll_ret < 0 && (errno == EINTR || errno == ENOMEM)) goto POLL_IN;
    if (!poll_ret) {
      if (!--retry_count) {
        error_msg("timeout");
        break;
      }
      timeout += 150;
      goto RETRY_SEND;
    } else if (poll_ret == 1) {
      len = read(pollfds[0].fd, rpkt, blksize + 4);
      if (len < 0) {
        send_errpkt(dstaddr, socklen, "read-error");
        break;
      }
      if (len < 4) goto POLL_IN;
    } else {
      perror_msg("poll");
      break;
    }
    // Validate receive packet.
    pktopcode = ntohs(((uint16_t*)rpkt)[0]);
    rblockno = ntohs(((uint16_t*)rpkt)[1]);
    if (pktopcode == TFTPD_OP_ERR) {
      char *message = "DATA Check failure.";
      char *arr[] = {"File not found", "Access violation",
        "Disk full or allocation exceeded", "Illegal TFTP operation",
        "Unknown transfer ID", "File already exists",
        "No such user", "Terminate transfer due to option negotiation"};

      if (rblockno && (rblockno < 9)) message = arr[rblockno - 1];
      error_msg(message);
      break; // Break the for loop.
    }

    // if download requested by client,
    // server will send data pkt and will receive ACK pkt from client.
    if ((opcode == TFTPD_OP_RRQ) && (pktopcode == TFTPD_OP_ACK)) {
      if (rblockno == (uint16_t) (blockno - 1)) {
        if (!done) continue; // Send next chunk of data.
        break;
      }
    }
    
    // server will receive DATA pkt and write the data.
    if ((opcode == TFTPD_OP_WRQ) && (pktopcode == TFTPD_OP_DATA)) {
      if (rblockno == blockno) {
        int nw = writeall(fd, &rpkt[4], len-4);
        if (nw != len-4) {
          g_errpkt[3] = TFTPD_ER_FULL;
          send_errpkt(dstaddr, socklen, "write error");
          break;
        }
      
        if (nw != blksize) done = 1;
      }
      continue;
    }
    goto POLL_IN;
  } // end of loop

CLEAN_APP:
  if (CFG_TOYBOX_FREE) {
    free(spkt);
    free(rpkt);
    close(fd);
  }
}

void tftpd_main(void)
{
  int recvmsg_len, rbuflen, opcode, blksize = TFTPD_BLKSIZE, tsize = 0, set =1;
  struct sockaddr_storage srcaddr, dstaddr;
  socklen_t socklen = sizeof(struct sockaddr_storage);
  char *buf = toybuf;

  TT.pw = NULL;
  memset(&srcaddr, 0, sizeof(srcaddr));
  if (getsockname(STDIN_FILENO, (struct sockaddr*)&srcaddr, &socklen)) {
    toys.exithelp = 1;
    error_exit(NULL);
  }

  if (TT.user) TT.pw = xgetpwnam(TT.user);
  if (*toys.optargs) {
    if (chroot(*toys.optargs))
      perror_exit("can't change root directory to '%s'", *toys.optargs);
    if (chdir("/")) perror_exit("can't change directory to '/'");
  }

  recvmsg_len = recvfrom(STDIN_FILENO, toybuf, TFTPD_BLKSIZE, 0,
      (struct sockaddr*)&dstaddr, &socklen);

  TT.sfd = xsocket(((struct sockaddr*)&dstaddr)->sa_family, SOCK_DGRAM, 0);
  if (setsockopt(TT.sfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&set,
        sizeof(set)) < 0) perror_exit("setsockopt failed");
  if (bind(TT.sfd, (struct sockaddr*)&srcaddr, socklen)) perror_exit("bind");
  if (connect(TT.sfd, (struct sockaddr*)&dstaddr, socklen) < 0)
    perror_exit("can't connect to remote host");
  // Error condition.
  if (recvmsg_len < 4 || recvmsg_len > TFTPD_BLKSIZE
		  || toybuf[recvmsg_len-1] != '\0') {
    send_errpkt((struct sockaddr*)&dstaddr, socklen, "packet format error");
    return;
  }

  // request is either upload or Download.
  opcode = ntohs(*(uint16_t*)buf);
  if (((opcode != TFTPD_OP_RRQ) && (opcode != TFTPD_OP_WRQ))
      || ((opcode == TFTPD_OP_WRQ) && (toys.optflags & FLAG_r))) {
    send_errpkt((struct sockaddr*)&dstaddr, socklen,
    	((opcode == TFTPD_OP_WRQ) ? "write error" : "packet format error"));
    return;
  }

  buf += 2;
  if (*buf == '.' || strstr(buf, "/.")) {
    send_errpkt((struct sockaddr*)&dstaddr, socklen, "dot in filename");
    return;
  }

  buf += strlen(buf) + 1; //1 '\0'.
  // As per RFC 1350, mode is case in-sensitive.
  if ((buf >= (toybuf + recvmsg_len)) || (strcasecmp(buf, "octet"))) {
    send_errpkt((struct sockaddr*)&dstaddr, socklen, "packet format error");
    return;
  }

  //RFC2348. e.g. of size type: "ttype1\0ttype1_val\0...ttypeN\0ttypeN_val\0"
  buf += strlen(buf) + 1;
  rbuflen = toybuf + recvmsg_len - buf;
  if (rbuflen) {
    int jump = 0, bflag = 0;

    for (; rbuflen; rbuflen -= jump, buf += jump) {
      if (!bflag && !strcasecmp(buf, "blksize")) { //get blksize
        errno = 0;
        blksize = strtoul(buf, NULL, 10);
        if (errno || blksize > 65564 || blksize < 8) blksize = TFTPD_BLKSIZE;
        bflag ^= 1;
      } else if (!tsize && !strcasecmp(buf, "tsize")) tsize ^= 1;
      
      jump += strlen(buf) + 1;
    }
    tsize &= (opcode == TFTPD_OP_RRQ);
  }

  //do send / receive file.
  do_action((struct sockaddr*)&srcaddr, (struct sockaddr*)&dstaddr,
      socklen, toybuf + 2, opcode, tsize, blksize);
  if (CFG_TOYBOX_FREE) close(STDIN_FILENO);
}