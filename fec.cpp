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

#include "ltga.h"

 #include <string.h>
#include <algorithm>
#include <iterator>

using namespace std;

/*
 * fec_init(): initialize the FEC data by copying the provided "imgseg"
 * into the provided "fecdata".  If "segsize" is smaller than "datasize",
 * fill the remainder of "fecdata" with 0s.
*/
void
fec_init(unsigned char *fecdata, unsigned char *imgseg, int datasize, int segsize)
{
  /* YOUR CODE HERE */
  memset(fecdata, 0, datasize); //zero-out fecdata
	memcpy(fecdata, imgseg, segsize);	//copy in only as big as we have data for - rest are zeros
  return;
}

/*
 * fec_accum(): accumulate the provided "imgseg" into the provided "fecdata"
 * by XOR them.  If "segsize" is smaller than "datasize", XOR the
 * remainder of "fecdata" with 0s.
*/
void
fec_accum(unsigned char *fecdata, unsigned char *imgseg, int datasize, int segsize)
{
  /* YOUR CODE HERE */
	unsigned char *tmp = new unsigned char[datasize];
	memset(tmp, 0, datasize);
	memcpy(tmp, imgseg, segsize);
	transform(tmp, tmp+datasize, fecdata, fecdata, bit_xor<unsigned char>());
	//*fecdata^=*tmp;
  return;
}

