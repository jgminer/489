/*
 * Copyright (c) 2014 University of Michigan, Ann Arbor.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of Michigan, Ann Arbor. The name of the University 
 * may not be used to endorse or promote products derived from this 
 * software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Author: Sugih Jamin (jamin@eecs.umich.edu)
 *
*/
#include <stdio.h>         // fprintf(), perror(), fflush()
#include <stdlib.h>        // atoi(), random()
#include <assert.h>        // assert()
#include <limits.h>        // LONG_MAX, INT_MAX
#include <iostream>
using namespace std;
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>      // socklen_t
#include "wingetopt.h"
#else
#include <string.h>        // memset(), memcmp(), strlen(), strcpy(), memcpy()
#include <unistd.h>        // getopt(), STDIN_FILENO, gethostname()
#include <signal.h>        // signal()
#include <netdb.h>         // gethostbyname(), gethostbyaddr()
#include <netinet/in.h>    // struct in_addr
#include <arpa/inet.h>     // htons(), inet_ntoa()
#include <sys/types.h>     // u_short
#include <sys/socket.h>    // socket API, setsockopt(), getsockname()
#include <sys/ioctl.h>     // ioctl(), FIONBIO
#endif
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "ltga.h"
#include "netimg.h"
#include "fec.h"

void
imgdb_usage(char *progname)
{
  fprintf(stderr, "Usage: %s [-d <drop probability>]\n", progname); 
  exit(1);
}

/*
 * imgdb_args: parses command line args.
 *
 * Returns 0 on success or 1 on failure.  On successful return,
 * the provided drop probability is copied to memory pointed to by
 * "pdrop", which must be allocated by caller.  
 *
 * Nothing else is modified.
 */
int
imgdb_args(int argc, char *argv[], float *pdrop)
{
  char c;
  extern char *optarg;

  if (argc < 1) {
    return (1);
  }
  
  *pdrop = NETIMG_PDROP;

  while ((c = getopt(argc, argv, "d:")) != EOF) {
    switch (c) {
    case 'd':
      *pdrop = atof(optarg);
      if (*pdrop > 0.0 && (*pdrop > 0.11 || *pdrop < 0.051)) {
        fprintf(stderr, "%s: recommended drop probability between 0.011 and 0.51.\n", argv[0]);
      }
      break;
    default:
      return(1);
      break;
    }
  }

  return (0);
}

/*
 * imgdb_imginit: load TGA image from file to *image.
 * Store size of image, in bytes, in *img_size.
 * Initialize *imsg with image's specifics.
 * All three variables must point to valid memory allocated by caller.
 * Terminate process on encountering any error.
 */
void
imgdb_imginit(char *fname, LTGA *image, imsg_t *imsg, int *img_size)
{
  int alpha, greyscale;
  double img_dsize;
  
  imsg->im_vers = NETIMG_VERS;
  imsg->im_type = NETIMG_DIM;

  image->LoadFromFile(fname);

  if (!image->IsLoaded()) {
    imsg->im_found = 0;
  } else {
    imsg->im_found = 1;

    cout << "Image: " << endl;
    cout << "     Type   = " << LImageTypeString[image->GetImageType()] 
         << " (" << image->GetImageType() << ")" << endl;
    cout << "     Width  = " << image->GetImageWidth() << endl;
    cout << "     Height = " << image->GetImageHeight() << endl;
    cout << "Pixel depth = " << image->GetPixelDepth() << endl;
    cout << "Alpha depth = " << image->GetAlphaDepth() << endl;
    cout << "RL encoding  = " << (((int) image->GetImageType()) > 8) << endl;
    /* use image->GetPixels()  to obtain the pixel array */
    
    img_dsize = (double) (image->GetImageWidth()*image->GetImageHeight()*(image->GetPixelDepth()/8));
    net_assert((img_dsize > (double) NETIMG_MAXSEQ), "imgdb: image too big");
    *img_size = (int) img_dsize;

    imsg->im_depth = (unsigned char)(image->GetPixelDepth()/8);
    imsg->im_width = htons(image->GetImageWidth());
    imsg->im_height = htons(image->GetImageHeight());
    alpha = image->GetAlphaDepth();
    greyscale = image->GetImageType();
    greyscale = (greyscale == 3 || greyscale == 11);
    if (greyscale) {
      imsg->im_format = htons(alpha ? GL_LUMINANCE_ALPHA : GL_LUMINANCE);
    } else {
      imsg->im_format = htons(alpha ? GL_RGBA : GL_RGB);
    }
  }

  return;
}
  
/*
 * imgdb_sockinit: sets up a UDP server socket.
 * Let the call to bind() assign an ephemeral port to the socket.
 * Determine and print out the assigned port number to screen so that user
 * would know which port to use to connect to this server.
 *
 * Terminates process on error.
 * Returns the bound socket id.
*/
int
imgdb_sockinit()
{
  int sd=-1;
  int err, len;
  struct sockaddr_in self;
  char sname[NETIMG_MAXFNAME+1] = { 0 };

#ifdef _WIN32
  WSADATA wsa;

  err = WSAStartup(MAKEWORD(2,2), &wsa);  // winsock 2.2
  net_assert(err, "imgdb: WSAStartup");
#endif

  /* create a UDP socket */
  /* Lab 5: YOUR CODE HERE */
  if ((sd = socket(AF_INET, SOCK_DGRAM, 0)) < 0){
    perror("opening UDP socket");
    abort();
  }
  
  
  memset((char *) &self, 0, sizeof(struct sockaddr_in));
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = INADDR_ANY;
  self.sin_port = 0;

  /* bind address to socket */
  err = bind(sd, (struct sockaddr *) &self, sizeof(struct sockaddr_in));
  net_assert(err, "imgdb_sockinit: bind");

  /*
   * Obtain the ephemeral port assigned by the OS kernel to this
   * socket and store it in the local variable "self".
   */
  len = sizeof(struct sockaddr_in);
  err = getsockname(sd, (struct sockaddr *) &self, (socklen_t *) &len);
  net_assert(err, "imgdb_sockinit: getsockname");

  /* Find out the FQDN of the current host and store it in the local
     variable "sname".  gethostname() is usually sufficient. */
  err = gethostname(sname, NETIMG_MAXFNAME);
  net_assert(err, "imgdb_sockinit: gethostname");

  /* inform user which port this peer is listening on */
  fprintf(stderr, "imgdb address is %s:%d\n", sname, ntohs(self.sin_port));

  return sd;
}

/* 
 * imgdb_recvquery: receive an iqry_t packet and store
 * the client's address and port number in the provided
 * "client" argument.  Check that the incoming iqry_t packet
 * is of version NETIMG_VERS and of type NETIMG_SYN.  If so,
 * save the mss and rwnd and image name information into the
 * provided "mss", "rwnd", "fwnd", and "fname" arguments respectively.
 * The provided arguments must all point to pre-allocated space.
 *
 * If error encountered when receiving packet or if packet is
 * of the wrong version or type, throw it away and wait for another
 * packet.  Loop until an iqry_t packet is received.
 *
 * Nothing else is modified.
*/
void
imgdb_recvquery(int sd, struct sockaddr_in *client, unsigned short *mss,
                unsigned char *rwnd, unsigned char *fwnd, char *fname)
{
  /* Receive an iqry_t packet as you did in Lab 5 and Lab6,
   * however, if the arriving packet is not an iqry_t packet, throw it
   * away and wait for another packet.  Loop until an iqry_t packet is
   * received.  When an iqry_t packet is received, save the mss, rwnd,
   * and fwnd and return to caller.
  */

  /* Task 2.1: if the arriving message is a valid query message,
  return to caller, otherwise, loop again until a valid query
  message is received. */
  /* Lab 6: YOUR CODE HERE */ 
  while(1){
    iqry_t tmpqry = {0,0,0,0};
    socklen_t clientsize = sizeof(sockaddr_in);
    int recv_bytes = -1;
    recv_bytes = recvfrom(sd, &tmpqry, sizeof(iqry_t), 0, (sockaddr *) client, &clientsize);
    //  cout << "type: " << tmpqry.iq_type << endl;
    if ((recv_bytes <= 0) || (tmpqry.iq_vers != NETIMG_VERS) || (tmpqry.iq_type != NETIMG_SYN)){
      // cout << "waiting: nothing received or error or incorrect type" << endl;
      continue;
    }
    *mss = ntohs(tmpqry.iq_mss);
    *rwnd = tmpqry.iq_rwnd;
    strcpy(fname, tmpqry.iq_name);
    if ((tmpqry.iq_vers == NETIMG_VERS) || (tmpqry.iq_type == NETIMG_SYN)){
      cout << "received proper-form query" << endl;
      break;
    }
  }
  return;
} 
  
/* 
 * imgdb_sendpkt: sends the provided "pkt" of size "size"
 * to the client "client" and wait for an ACK packet.
 * If ACK doesn't return before retransmission timeout,
 * re-send the packet.  Keep on trying for NETIMG_MAXTRIES times.
 *
 * Returns 0 on success: pkt sent without error and ACK returned.
 * Else, return 1 if ACK is not received.
 * Terminate process on send or receive error.
 *
 * Nothing else is modified.
*/
int
imgdb_sendpkt(int sd, struct sockaddr_in *client, char *pkt, int size, ihdr_t *ack)
{
  /* Task 2.1: sends the provided pkt to client as you did in Lab 5.
   * In addition, initialize a struct timeval timeout variable to
   * NETIMG_SLEEP sec and NETIMG_USLEEP usec and wait
   * for read event on socket sd up to the timeout value.
   * If no read event occurs before the timeout, try sending
   * the packet to client again.  Repeat NETIMG_MAXTRIES times.
   * If read event occurs before timeout, receive the incoming
   * packet and make sure that it is an ACK pkt as expected.
   */
  /* YOUR CODE HERE */
  int err = 0;
  struct timeval tv = {0,0};
  fd_set rset;

  tv.tv_sec = NETIMG_SLEEP;
  tv.tv_usec = NETIMG_USLEEP;

  FD_ZERO(&rset);
  FD_SET(sd, &rset);

  err = sendto(sd, pkt, size, 0, (sockaddr *) client, sizeof(sockaddr_in)); //TODO: double check size of pkt
  if (err <= 0){
    cout << "error in sending imsg" << endl;
    abort();
  }

  for (int i = 0; i < NETIMG_MAXTRIES; i++){
    err = select(sd+1, &rset, 0, 0, &tv);
    // cout << "result of select: " << err << endl;
    if (err > 0){
      err = recv(sd, (char *) ack, sizeof(ihdr_t), 0);   // imsg global
      if (err <= 0) {
        //close(sd);
        continue;
      }
      //convert seqn back to host order for assert following
      ack->ih_seqn = ntohl(ack->ih_seqn);
      return(0);
    }
  }

  return(1);
}

/*
 * imgdb_sendimage: 
 * Send the image contained in *image to the client
 * pointed to by *client. Send the image in
 * chunks of segsize, not to exceed mss, instead of
 * as one single image. With probability pdrop, drop
 * a segment instead of sending it.
 *
 * If receive wrong packet type while waiting for ACK,
 * assume client has exited, and simply return to caller.
 *
 * Terminate process upon encountering any other error.
 * Doesn't otherwise modify anything.
*/
void
imgdb_sendimage(int sd, struct sockaddr_in *client, unsigned short mss,
                unsigned char rwnd, unsigned char fwnd,
                LTGA *image, int img_size, float pdrop)
{
  unsigned char *ip;

  int datasize = 0;
  int left = 0;
  unsigned short segsize = 0;

  ip = image->GetPixels();    /* ip points to the start of byte buffer holding image */
  datasize = mss - sizeof(ihdr_t) - NETIMG_UDPIPSIZE;

  unsigned char *fecdata = new unsigned char[datasize];

  /* make sure that the send buffer is of size at least mss. */
  /* Lab 5: YOUR CODE HERE */
  int i_mss = mss;
  cout << "mss is: " << i_mss << endl;
  int err = setsockopt(sd, SOL_SOCKET, SO_SNDBUF, &i_mss, sizeof(i_mss));
  net_assert(err==-1, "setting buffer size");

  /* 
   * Populate a struct msghdr with information of the destination client,
   * a pointer to a struct iovec array.  The iovec array should be of size
   * NETIMG_NUMIOVEC.  The first entry of the iovec should be initialized
   * to point to an ihdr_t, which should be re-used for each chunk of data
   * to be sent.
  */
  /* Lab 5: YOUR CODE HERE */

  struct msghdr msg_hdr;
  memset(&msg_hdr, 0, sizeof(msghdr));
  ihdr_t ihdr= {NETIMG_VERS,NETIMG_DAT,0,0}; //init data ihdr
  iovec vec[NETIMG_NUMIOVEC];
  msg_hdr.msg_iov = vec;
  //vector<iovec> *iovecptr;

  msg_hdr.msg_name = client; //TODO: not self, right?
  msg_hdr.msg_namelen = sizeof(sockaddr_in);
  msg_hdr.msg_iovlen = NETIMG_NUMIOVEC;

  msg_hdr.msg_iov[0].iov_base = &ihdr;
  msg_hdr.msg_iov[0].iov_len = sizeof(ihdr_t);

  /* Task 2.2 and Task 4.1: initialize any necessary variables
   * for your sender side sliding window and FEC window.
   */
  /* YOUR CODE HERE */
  // int sent_bytes = 0;
  // int acked_bytes = 0;
  int snd_una = 0;
  int snd_next = 0;
  // cout << "receiver's window is: " << (int)rwnd << endl;
  int wnd_size = rwnd;
  int usable = 0;
  int window_sent = 0;

  int fec_count = 0;


  do {
    /* Task 2.2: estimate the receiver's receive buffer based on packets
     * that have been sent and ACKed.  We can only send as much as the
     * receiver can buffer.
     */
    /* YOUR CODE HERE */
    //TODO:
    //use datasize here since we aren't accounting for the ihdr_t and udp headers
    usable = wnd_size*datasize - (snd_next - snd_una);

    /* size of this segment */
    //snd_next may be equal to snd_una
    left = img_size - snd_next;
    segsize = datasize > left ? left : datasize;

    

    /* Task 2.2: Send one usable window-full of data to client using
     * sendmsg() to send each segment as you did in Lab5.  As in Lab
     * 5, you probabilistically drop a segment instead of sending it.
     * Basically, copy the code within the do{}while loop in
     * imgdb_sendimage() from Lab 5 here, but put it within another
     * loop such that a usable window-full of segments can be sent out
     * using the Lab 5 code.
     *
     * Task 4.1: Before you send out each segment, update your FEC variables and
     * initialize or accumulate your FEC data packet by copying your
     * Lab 6 code here appropriately.
     *
     * Task 4.1: After you send out each segment, if you have accumulated an FEC
     * window full of segments or the last segment has been sent, send
     * your FEC data.  Again, copy your Lab 6 code here to the
     * appropriate place.
     */
    /* YOUR CODE HERE */


    while((window_sent < usable) && (snd_next <= img_size)){

      //TODO: may not be correct??
      if (fec_count == 0) fec_init(fecdata, ip+snd_next, datasize, segsize);
      else fec_accum(fecdata, ip+snd_next, datasize, segsize);
      cout << "accum on: 0x" << hex << snd_next << endl;

           

      if (((float) random())/INT_MAX < pdrop) {
      fprintf(stderr, "imgdb_sendimage: DROPPED offset 0x%x, 0x%x bytes, unacked: 0x%x\n",
              snd_next, segsize, snd_una);
        snd_next += datasize;
        window_sent += datasize;
        fec_count++;
      } 

      else {
        msg_hdr.msg_iov[1].iov_base = ip+snd_next;
        msg_hdr.msg_iov[1].iov_len = segsize;

        //update header
        ihdr.ih_size = htons(segsize);
        ihdr.ih_seqn = htonl(snd_next);

        err = sendmsg(sd, &msg_hdr, 0);
        if (err <= 0){
          net_assert(err<=0, "error in sendmsg - IMGDATA");
          abort();
        }

        fprintf(stderr, "imgdb_sendimage: sent offset 0x%x, 0x%x bytes, unacked: 0x%x\n",
                snd_next, segsize, snd_una);
        snd_next += datasize;
        window_sent += datasize;

        fec_count++;
      }

      if (fec_count == 11 || snd_next > img_size || window_sent >= usable){
        //update header with next windows' seq number - incremented above
        ihdr.ih_seqn = htonl(snd_next);
        ihdr.ih_size = htons(datasize);

        ihdr.ih_type = NETIMG_FEC;

        //update iovec second entry to point to FEC data
        msg_hdr.msg_iov[1].iov_base = fecdata;
        msg_hdr.msg_iov[1].iov_len = datasize;

        fprintf(stderr, "imgdb_sendimage: sent FEC offset 0x%x, segment count: %d\n",
                snd_next, fec_count);
        err = sendmsg(sd, &msg_hdr, 0);
        if (err <= 0){
          net_assert(err<=0, "error in sendmsg - FEC");
          abort();
        }


        ihdr.ih_type = NETIMG_DAT; //reset type
        memset(fecdata, 0, datasize);
        fec_count = 0;
      }
    }

    /* Task 2.2: Next wait for ACKs for up to NETIMG_SLEEP secs and NETIMG_USLEEp usec. */
    /* YOUR CODE HERE */

    //DONE BELOW????

    /* Task 2.2: If an ACK arrived, grab it off the network and slide
     * our window forward when possible. Continue to do so for all the
     * ACKs that have arrived.  Remember that we're using cumulative ACK.
     *
     * We have a blocking socket, but here we want to
     * opportunistically grab all the ACKs that have arrived but not
     * wait for the ones that have not arrived.  So, when we call
     * receive, we want the socket to be non-blocking.  However,
     * instead of setting the socket to non-blocking and then set it
     * back to blocking again, we simply set flags=MSG_DONTWAIT when
     * calling the receive function.
     */
    /* YOUR CODE HERE */
    ihdr_t this_ack = {0,0,0,0}; //TODO: OK outside?

    struct timeval tv = {0,0};
    fd_set rset;

    tv.tv_sec = NETIMG_SLEEP;
    tv.tv_usec = NETIMG_USLEEP;

    FD_ZERO(&rset);
    FD_SET(sd, &rset);

    while(1){
      int recv_bytes = -1;
      err = select(sd+1, &rset, 0, 0, &tv);
      // cout << "result of select: " << err << endl;
      if (err>0){
        recv_bytes = recv(sd, &this_ack, sizeof(ihdr_t), MSG_DONTWAIT);

        if (recv_bytes == 0){
          cout << "no more acks" << endl;
          break;
        }
        else if (recv_bytes < 0){
          cout << "no ack?" << endl;
          break;
        }
        //set snd_una to this ack
        else {
          printf("imgdb_sendimage: received ack: 0x%x, unacked was: 0x%x, send next 0x%x\n",
           ntohl(this_ack.ih_seqn), snd_una, snd_next);
          snd_una = ntohl(this_ack.ih_seqn);
        }
        // break;
      }
      else break;

    }

    /* Task 2.2: If no ACK returned up to the timeout time, trigger Go-Back-N
     * and re-send all segments starting from the last unACKed segment.
     *
     * Task 4.1: If you experience RTO, reset your FEC window to start
     * at the segment to be retransmitted.
    */
    /* YOUR CODE HERE */
    if (this_ack.ih_vers == 0){
      printf("imgdb_sendimage: RTO unacked: 0x%x, next offset 0x%x, usable %d bytes\n",
       snd_una, snd_next, (usable-window_sent)); 

      snd_next = snd_una; //TODO: this simple??
      window_sent = 0;
      fec_count = 0;
    }
    else if ((snd_una < img_size) && (snd_next > img_size)){
      cout << "here once, at end if necessary" << endl;
      snd_next = snd_una;
      window_sent = 0;
      fec_count = 0;
    }
          
    // cout << "snd_next: " << snd_next << endl;
    //TODO: less than????
  } while (snd_next <= img_size); // Task 2.2: replace the '1' with your condition for detecting 
               // that all segments sent have been acknowledged

  
  /* Task 2.2: after the image is sent
   * send a NETIMG_FIN packet and wait for ACK,
   * using imgdb_sendpkt().
   */
  /* YOUR CODE HERE */
  for (int i = 0; i < NETIMG_MAXTRIES; i++){
    ihdr_t ack = {0,0,0,0};
    //TODO: convert and add size?????
    ihdr_t FIN = {NETIMG_VERS, NETIMG_FIN, 0, NETIMG_FINSEQ};
    printf("imgdb_sendimage: sent FIN (0x%x), unacked: 0x%x", (int)NETIMG_FINSEQ, snd_una); 
    FIN.ih_seqn = htonl(FIN.ih_seqn); //convert to network order before sending
    err = imgdb_sendpkt(sd, client, (char *)&FIN, sizeof(ihdr_t), &ack);
    if(err == 0){
      break;
    }
  }
  
  return;
}

int
main(int argc, char *argv[])
{ 
  int sd;
  float pdrop=NETIMG_PDROP;
  LTGA image;
  imsg_t imsg;
  ihdr_t ack;
  int img_size;
  struct sockaddr_in client;
  unsigned short mss;
  unsigned char rwnd, fwnd;
  char fname[NETIMG_MAXFNAME] = { 0 };

#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);    /* don't die if peer is dead */
#endif

  // parse args, see the comments for imgdb_args()
  if (imgdb_args(argc, argv, &pdrop)) {
    imgdb_usage(argv[0]);
  }

  srandom(48914+(int)(pdrop*100));
  sd = imgdb_sockinit();
  
  while (1) {
    imgdb_recvquery(sd, &client, &mss, &rwnd, &fwnd, fname);
    imgdb_imginit(fname, &image, &imsg, &img_size);
    if (imgdb_sendpkt(sd, &client, (char *) &imsg, sizeof(imsg_t), &ack)) {
      continue;
    }
    net_assert((ack.ih_seqn != NETIMG_DIMSEQ), "imgdb: sendimsg received wrong ACK seqn");
    imgdb_sendimage(sd, &client, mss, rwnd, fwnd, &image, img_size, pdrop);
  }
    
  //close(sd);
#ifdef _WIN32
  WSACleanup();
#endif
  exit(0);
}
