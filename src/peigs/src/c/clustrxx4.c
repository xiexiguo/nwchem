/*======================================================================
 *
 * DISCLIMER
 *
 * This material was prepared as an account of work sponsored by an
 * agency of the United States Government.  Neither the United States
 * Government nor the United States Department of Energy, nor Battelle,
 * nor any of their employees, MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR
 * ASSUMES ANY LEGAL LIABILITY OR RESPONSIBILITY FOR THE ACCURACY,
 * COMPLETENESS, OR USEFULNESS OF ANY INFORMATION, APPARATUS, PRODUCT,
 * SOFTWARE, OR PROCESS DISCLOSED, OR REPRESENTS THAT ITS USE WOULD NOT
 * INFRINGE PRIVATELY OWNED RIGHTS.
 *
 * ACKNOWLEDGMENT
 *
 * This software and its documentation were produced with Government
 * support under Contract Number DE-AC06-76RLO-1830 awarded by the United
 * States Department of Energy.  The Government retains a paid-up
 * non-exclusive, irrevocable worldwide license to reproduce, prepare
 * derivative works, perform publicly and display publicly by or for the
 * Government, including the right to distribute to other Government
 * contractors.
 *
 *======================================================================
 *
 *  -- PEIGS  routine (version 2.1) --
 *     Pacific Northwest Laboratory
 *     July 28, 1995
 *
 *======================================================================
 */
/*
  PeIGS internal routine:
  
  not intended for external use; see the routine pstein
  
  Integer clustrinv_(n, d, e, eval, schedule, num_clustr, mapZ, mapvecZ, vecZ, imin, nacluster, icsplit, iscratch, scratch)
  
  Integer *n, *schedule, *num_clustr, *mapZ, *mapvecZ, *Zbegn, *nacluster,
          *icsplit, *iscratch;
  
  DoublePrecision *d, *e, *eval, **vecZ, *scratch;
  
  
  for computing eigenvectors by inverse iteration with cluster modified Gram Schmidt
  with "de-tangling" of clusters.

  returns 0     if everything seems ok.
          k > 0 if k-th eigenvector (owned by processor mapZ[k-1], k = 1 to neigval,
                failed to converge in inverse iteration.  In this case eigenvectors
                1 to k-1 converged ok, eigenvector k failed to converge, and it is
                possible that some eigenvectors numbered greater than k failed to
                converge.  In any case, the first k-1 eigenvectors are good,
                while eigenvectors k is bad, and eigenvectors k+1 to neigval are
                probably bad too.  Note that the meaning of bad is not always clear,
                in general, however, it means that the eigenvector is probably
                not accurate to full precision.
                

  This code "de-tangles" clusters as follows.  Assume the eigenvalues in a cluster
  are owned by more than one processor (as determined by mapZ).  Then, the processor
  owing the first eigenvector in the cluster does all required iterations of
  inverse iteration and mgs to compute the eigenvectors in this cluster.  It then
  sends these finished eigenvectors to the second processor in mapZ which owns part
  of this cluster.  At this point the clusters are de-tangled and all processors can
  start working on their eigenvectors.
  
  This version of the code assumes that during de-tangling each processor owns the
  first eigenvector in at most one multi-processor cluster  and receives the finished
  eigenvalues from at most one other processor.  This assumptions are satified by
  the current algorithm in clustrf and are checkd by this routine.

  */

#include <stdio.h>
#include <math.h>

#include "globalp.c.h"
#include "clustr_inv.h"

/*
#define DEBUG1
#define DEBUG5
*/
#define CLUSTRLEN  4
#define LOOP  3
#define INV_TIME 1
#define ITIME1  1

Integer clustrinv4_(n, d, e, dplus, lplus, ld, lld, eval, schedule, num_clustr, mapZ, mapvecZ, vecZ, imin, nacluster, icsplit, iscratch, scratch)
     Integer *n, *schedule, *num_clustr, *mapZ, *mapvecZ, *imin,
  *nacluster, *icsplit, *iscratch;
     DoublePrecision *d, *e, *dplus, *lplus, *ld, *lld, *eval, **vecZ, *scratch;
{
  /*
    n = dimension of the tridiagonal matrix
    d = diagonal of the tridiagonal matrix
    e[2:n] = upper diagonal of the tridiagonal matrix
    schedule = a integer array of the scheduling information
    mapZ = location of the eigenvectors
    mapvecZ = in
    vecZ
    iscratch = integer scratch space
    scratch = DoublePrecision precision scratch space
    
    returns the number of eigenvector that this processor holds
    */
  
  static Integer three = 3, IONE = 1;
  Integer indx, i, j, iseed[4], bb1, bn;
  Integer blksiz, clustr_ptr, cn;
  Integer me, naproc, Zvec;
  Integer *cl_ptr;
  Integer c1, csiz, xc1, xcsiz, xblksiz;
  Integer cl_num;
  Integer itime;
  Integer send_num, send_cl, send_to,
          recv_num, recv_cl, recv_from,
          myindx, ime, itype, nvecs, isize, ival, first, ibad, itmp;
  
  DoublePrecision stpcrt, onenrm, eps;
  DoublePrecision tmp, *dscrat, *first_buf;
  
  extern void xerbla_();
  extern Integer idamax_(), mclock_(), succ_(), mxmynd_(), mxnprc_();
  extern DoublePrecision dasum_(), dnrm2_(), ddot_(), dlarnd_();
  extern void dlagtf_(), dlarnv_();
  extern void printff_(), mgs_3();
  extern void mgspnl_(), dscal_(), dlagts_();
  extern void dcopy_(), daxpy_();
  extern Integer count_list ();
  extern Integer inv_it();
  extern void mgs ();
  extern void fil_dbl_lst ();
  
  /*
    Function Body
    */
  
  me = mxmynd_();
  naproc = mxnprc_();

  ibad = 0;

  dscrat = scratch;

  /*
    Get machine constants. should set this up somewhere to call it only once
    */

  eps = DLAMCHE;
  
  /*
    Initialize seed for random number generator DLARND.
    how about randomizing the randomness?
    */
  
  blksiz = 1;

  for (i = 0; i < 4; ++i)
    iseed[i] = 1;
  
  iseed[3] = 2*me + 1;
  
  /*
    Compute eigenvectors of matrix blocks.
    */
  
  /*
    this is the number of clusters that this processor will process
    */
  

  cl_num = *num_clustr;
  Zvec = 0;
  
  if ( cl_num == 0 )
    return(ibad);

  /*
   * If I own the start of my last cluster, but not all of this cluster
   * then do my part of the last cluster and send it to the processor
   * which owns the first vector in the cluster which is not mine.
   */

  cl_ptr = schedule;

  send_num = 0; 
  recv_num = 0;
  if( naproc > 1 ) {
    for (clustr_ptr= 0;  clustr_ptr < cl_num ; clustr_ptr++) {
      c1 = *(cl_ptr++);
      cn = *(cl_ptr++);
      bb1 = *(cl_ptr++);
      bn = *(cl_ptr++);

      if( cn > c1 ) {
        if( mapZ[c1] == me ) {
          for (j = c1+1; j <= cn; j++ ){
            if ( mapZ[j] != me ) {
              send_num++;
              send_cl = clustr_ptr;
              send_to = mapZ[j];
              break;
            }
          }
        }
        else {
          for (j = c1+1; j <= cn; j++ ){
            if ( mapZ[j] != mapZ[c1] ) {
              if ( mapZ[j] == me ) {
                recv_num++;
                recv_cl   = clustr_ptr;
                recv_from = mapZ[c1];
              }
              break;
            }
          }
        }
      }
    }

    /*
     *
     * The following tests verify that the "de-tangling" assumptions about
     * sending and/or receiving at most one finished set of eigenvectors listed
     * in the subroutine header are satisfied.
     *
     * The exit(-1) conditions should only occur if the
     * cluster scheduling algorithm is changed from its current form without
     * modifying the de-tangeling code appropriately.  Thus, the following exits
     * should never be executed in the tested, distribution version of the code.
     * Thus, we do not try to do a global exit here.
     *
     */

    if( send_num > 1 || recv_num > 1 ) {
      fprintf( stderr, " me = %d Internal Error in PEIGS clustrinv. \n", me );
      fprintf( stderr, " me = %d recv_num and/or send_num > 1. \n", me);
      exit( -1 );
    }
    if( ( send_num > 0 ) && ( send_cl != cl_num - 1) ) {
        fprintf( stderr, " me = %d Internal Error in PEIGS clustrinv. \n", me );
        fprintf( stderr, " me = %d send_cl != cl_num - 1. \n", me);
        exit( -1 );
    }
    if( ( recv_num > 0 ) && ( recv_cl != 0 ) ) {
        fprintf( stderr, " me = %d Internal Error in PEIGS clustrinv. \n", me );
        fprintf( stderr, " me = %d recv_cl != 0. \n", me);
        exit( -1 );
    }
  }

  if( recv_num > 0 || send_num > 0 ) {

    /* Set iscratch to a linked-list such that
     * iscratch[j] = k means processor
     * j receives from processor k when sending
     * the first block of a cluster to the next processor in that cluster.
     * k = -1 means processor j receives from no one.
     * Only really care about the part of the linked-list
     * including 'me'.
     */

    for (j= 0;  j < naproc; j++)
      iscratch[j] = -1;

    cn = -1;
    for( i = 0; i < *nacluster; i++ ) {
      c1 = cn + 1;
      cn = icsplit[i];
      if( cn > c1 ) {
        for (j = c1+1; j <= cn; j++ )
          if ( mapZ[j] != mapZ[c1] ) {
            iscratch[ mapZ[j] ] = mapZ[c1];
            break;
          }
      }
    }

    myindx = 0;
    j      = me;
    for (i= 0;  i < naproc; i++) {
      if( iscratch[j] == -1 ) 
        break;

      myindx++;
      j = iscratch[j];
    }

    ime = -1;
    for (i= c1;  i <= cn; i++) {
      ime ++;
      if ( mapZ[i] == me )
	break;
    }

#ifdef DEBUG11
	printf(" 44 me = %d ime = %d myindx = %d \n", me, ime, myindx);
#endif


    if( iscratch[j] != -1 ) {
      fprintf( stderr, " me = %d Internal Error in PEIGS clustrinv. \n", me );
      fprintf( stderr, " me = %d Swapping of initial cluster data \n", me);
      fprintf( stderr, " me = %d does not have a well defined start. \n", me);
      fprintf( stderr, " me = %d clustrinv,mgs cannot handle this. \n", me);
      exit( -1 );
    }

    if( send_num == 0 ) {
      itype = 91;
      c1     = schedule[4*recv_cl];
      csiz   = schedule[4*recv_cl+1] - c1 + 1;
      
      blksiz = schedule[4*recv_cl+3] - schedule[4*recv_cl+2] + 1;

      nvecs  = count_list( recv_from, &mapZ[c1], &csiz);

      isize  = sizeof( DoublePrecision ) * blksiz * nvecs;

      first_buf = dscrat;
      dscrat += nvecs * blksiz;

#ifdef DEBUG11
      printf(" me = %d 44 Just before mxread from %d  isize = %d nvecs = %d \n", me, recv_from, isize, nvecs );
      fflush(stdout);
#endif
      
      ival = mxread_( first_buf, &isize, &recv_from, &itype );
      
#ifdef DEBUG11
      printf(" me = %d 44 Just after mxread from %d \n", me, recv_from );
      fflush(stdout);
#endif
      
    }
  }

  cl_ptr = schedule;
  for (clustr_ptr= 0;  clustr_ptr < cl_num ; clustr_ptr++) {

    if( clustr_ptr == 0 && send_num > 0 ) {

        c1  = schedule[ 4*send_cl ];
        cn  = schedule[ 4*send_cl + 1 ];
        bb1 = schedule[ 4*send_cl + 2 ];
        bn  = schedule[ 4*send_cl + 3 ];

        if( clustr_ptr == send_cl )
          cl_ptr += 4;
    }
    else {

      c1 = *(cl_ptr++);
      cn = *(cl_ptr++);
      bb1 = *(cl_ptr++);
      bn = *(cl_ptr++);

      if( clustr_ptr == send_cl && send_num > 0 )
        cl_ptr += 4;

    }


    if ( c1 < *imin ) {
      Zvec = 0;
    }
    else {
      Zvec = c1 - *imin;
    }
    
    
    blksiz = bn - bb1 + 1;
    csiz = cn - c1 + 1;
    
    onenrm = fabs(d[bb1]) + fabs(e[bb1 + 1]);
    tmp = fabs(d[bn])+ fabs(e[bn]);
    onenrm = max(onenrm,tmp);
    
    for (i = bb1 + 1; i < bn; ++i){
      tmp = fabs(d[i])+fabs(e[i])+fabs(e[i + 1]);
      onenrm = max(onenrm, tmp );
    }
    
    stpcrt = sqrt((DoublePrecision ) 1.0e-1 / (DoublePrecision ) blksiz);
    
    for (i = 0; i < 4; ++i)
      iseed[i] = 1;
    iseed[3] = 1;
    
    indx = 0;
    
    first = 0;
    if( clustr_ptr == 0 && send_num > 0 )
      first = 1;
    
    itime = 1;
    for ( j = 0; j < INV_TIME; j++ ) {
      if ( c1 != cn ) {
	for ( i = 0; i < itime ; i++ ) {
	  mgs_3( &csiz, vecZ, &mapZ[c1], &bb1, &bn, &Zvec,
		 &first, first_buf, iscratch, dscrat);
	}
	itime = 1;
      }
    }
    
    
    /*
     * Swap beginning portions of clusters which are distributed
     * across more than one processor.
     */
    
    if( clustr_ptr == 0 && send_num > 0 ) {
      
      itype = 91;
      
      if( recv_num > 0  &&  (( myindx % 2 ) != 0 ) ) { 
	xc1     = schedule[4*recv_cl];
	xcsiz   = schedule[4*recv_cl+1] - xc1 + 1;
	xblksiz = schedule[4*recv_cl+3] - schedule[4*recv_cl+2] + 1;
	nvecs  = count_list( recv_from, &mapZ[xc1], &xcsiz);
	isize = sizeof( DoublePrecision ) * xblksiz * nvecs;
	
	first_buf = dscrat;
	dscrat += nvecs * xblksiz;
	
#ifdef DEBUG11
	printf(" me = %d Just before mxread from %d 2 isize = %d nvecs = %d \n", me, recv_from, isize, nvecs );
	fflush(stdout);
#endif
	
	ival = mxread_( first_buf, &isize, &recv_from, &itype );

#ifdef DEBUG11
	printf(" me = %d Just read from %d 2 isize = %d nvecs = %d \n", me, recv_from, isize, nvecs );
	fflush(stdout);
#endif

      }
      
      nvecs = 0;
      for (j = c1; j <= cn; j++ ){
        if ( mapZ[j] == me ) {
	  dcopy_(&blksiz, &vecZ[nvecs+Zvec][bb1], &IONE,
                 dscrat+nvecs*blksiz, &IONE );
          nvecs++;
        }
      }
      
      isize = sizeof( DoublePrecision ) * blksiz * nvecs;
#ifdef DEBUG11
      printf(" me = %d Just before mxwrit isize = %d nvecs = %d \n", me, isize, nvecs );
	fflush(stdout);
#endif
      
      ival = mxwrit_( dscrat, &isize, &send_to, &itype );

#ifdef DEBUG11
      printf(" me = %d wrote to %d \n", me, send_to);
	fflush(stdout);
#endif
      
      if( recv_num > 0  &&  (( myindx % 2 ) == 0 ) ) { 
        xc1     = schedule[4*recv_cl];
        xcsiz   = schedule[4*recv_cl+1] - xc1 + 1;
	
        xblksiz = schedule[4*recv_cl+3] - schedule[4*recv_cl+2] + 1;
	
        nvecs  = count_list( recv_from, &mapZ[xc1], &xcsiz);
        isize = sizeof( DoublePrecision ) * xblksiz * nvecs;
	
        first_buf = dscrat;
        dscrat += nvecs * xblksiz;
	
#ifdef DEBUG11
	printf(" me = %d Just before mxread frm %d isize = %d nvecs = %d \n", me, recv_from, isize, nvecs );
	fflush(stdout);
#endif
        ival = mxread_( first_buf, &isize, &recv_from, &itype );
#ifdef DEBUG11
	printf(" me = %d read from %d \n", me, recv_from );
	fflush(stdout);
#endif
      }
    }
  }
#ifdef DEBUG11
  printf(" me = %d Exiting clustrxx4 \n", me );
  fflush(stdout);
#endif
  
  return(ibad);
}
