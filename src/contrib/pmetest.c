/*
 * $Id$
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Gallium Rubidium Oxygen Manganese Argon Carbon Silicon
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "typedefs.h"
#include "macros.h"
#include "smalloc.h"
#include "copyrite.h"
#include "main.h"
#include "nrnb.h"
#include "txtdump.h"
#include "tpxio.h"
#include "statutil.h"
#include "futil.h"
#include "fatal.h"
#include "vec.h"
#include "ewald_util.h"
#include "nsb.h"
#include "pme.h"
#include "xvgr.h"
#include "pbc.h"
#include "mpi.h"
#include "block_tx.h"

rvec *xptr=NULL;

static int comp_xptr(const void *a,const void *b)
{
  int  va,vb;
  real dx;
  
  va = *(int *)a;
  vb = *(int *)b;
  
  if ((dx = (xptr[va][XX] - xptr[vb][XX])) < 0)
    return -1;
  else if (dx > 0)
    return 1;
  else
    return 0;
}

static void do_my_pme(FILE *fp,real tm,bool bVerbose,t_inputrec *ir,
		      rvec x[],rvec xbuf[],rvec f[],
		      real charge[],real qbuf[],matrix box,bool bSort,
		      t_commrec *cr,t_nsborder *nsb,t_nrnb *nrnb,
		      real ewaldcoeff,t_block *excl,real qtot,
		      int index[],FILE *fp_xvg)
{
  real   ener,vcorr,q,xx;
  tensor vir,vir_corr,vir_tot;
  rvec   mu_tot;
  int    i,m,ii;
  t_forcerec *fr;

  /* Initiate local variables */  
  snew(fr,1);
  fr->ewaldcoeff = ewaldcoeff;
  fr->f_el_recip = f;
  clear_mat(vir);
  clear_mat(vir_corr);
  
  /* Put x is in the box, this part needs to be parallellized properly */
  put_atoms_in_box(box,nsb->natoms,x);
  /* Here sorting of X (and q) is done.
   * Alternatively, one could just put the atoms in one of the
   * cr->nnodes slabs. That is much cheaper than sorting.
   */
  for(i=0; (i<nsb->natoms); i++)
    index[i] = i;
  if (bSort) {
    xptr = x;
    qsort(index,sizeof(index[0]),nsb->natoms,comp_xptr);
  }
  /* After sortimg we only need the part that is to be computed on 
   * this processor. We also compute the mu_tot here (system dipole)
   */
  clear_rvec(mu_tot);
  for(i=START(nsb); (i<START(nsb)+HOMENR(nsb)); i++) {
    ii      = index[i];
    q       = charge[ii];
    qbuf[i] = q;
    for(m=0; (m<DIM); m++) {
      xx         = x[ii][m];
      xbuf[i][m] = xx;
      mu_tot[m] += q*xx;
    }
    clear_rvec(f[ii]);
  }
  if (debug) {
    pr_rvec(debug,0,"qbuf",qbuf,nsb->natoms,TRUE);
    pr_rvecs(debug,0,"xbuf",xbuf,nsb->natoms);
    pr_rvecs(debug,0,"box",box,DIM);
  }  
  ener  = do_pme(fp,bVerbose,ir,xbuf,f,qbuf,box,cr,
		 nsb,nrnb,vir,ewaldcoeff,FALSE);
  vcorr = ewald_LRcorrection(fp,nsb,cr,fr,qbuf,excl,xbuf,box,mu_tot,qtot,
			     ir->ewald_geometry,ir->epsilon_surface,
			     vir_corr);
  gmx_sum(1,&ener,cr);
  gmx_sum(1,&vcorr,cr);
  fprintf(fp,"Time: %10.3f Energy: %12.5e  Correction: %12.5e  Total: %12.5e\n",
	  tm,ener,vcorr,ener+vcorr);
  if (fp_xvg) 
    fprintf(fp_xvg,"%10.3f %12.5e\n",tm,ener+vcorr);
  if (bVerbose) {
    m_add(vir,vir_corr,vir_tot);
    gmx_sum(9,vir_tot[0],cr);
    pr_rvecs(fp,0,"virial",vir_tot,DIM); 
  }
  fflush(fp);
}

int main(int argc,char *argv[])
{
  static char *desc[] = {
    "The pmetest program tests the scaling of the PME code. When only given",
    "a tpr file it will compute PME for one frame. When given a trajectory",
    "it will do so for all the frames in the trajectory. Before the PME",
    "routine is called the coordinates are sorted along the X-axis."
  };
  t_commrec    *cr,*mcr;
  static t_filenm fnm[] = {
    { efTPX, NULL,      NULL,       ffREAD  },
    { efTRN, "-o",      NULL,       ffWRITE },
    { efLOG, "-g",      "pme",      ffWRITE },
    { efTRX, "-f",      NULL,       ffOPTRD },
    { efXVG, "-x",      "ener-pme", ffWRITE }
  };
#define NFILE asize(fnm)

  /* Command line options ! */
  static bool bVerbose=FALSE;
  static bool bOptFFT=FALSE;
  static bool bSort=FALSE;
  static int  ewald_geometry=eewg3D;
  static int  nnodes=1;
  static int  nthreads=1;
  static int  pme_order=0;
  static rvec grid = { -1, -1, -1 };
  static real rc   = 0.0;
  static real dtol = 0.0;
  static t_pargs pa[] = {
    { "-np",      FALSE, etINT, {&nnodes},
      "Number of nodes, must be the same as used for grompp" },
    { "-nt",      FALSE, etINT, {&nthreads},
      "Number of threads to start on each node" },
    { "-v",       FALSE, etBOOL,{&bVerbose},  
      "Be loud and noisy" },
    { "-sort",    FALSE, etBOOL,{&bSort},  
      "Sort coordinates. Crucial for domain decomposition." },
    { "-grid",    FALSE, etRVEC,{&grid},
      "Number of grid cells in X, Y, Z dimension (if -1 use from tpr)" },
    { "-order",   FALSE, etINT, {&pme_order},
      "Order of the PME spreading algorithm" },
    { "-rc",      FALSE, etREAL, {&rc},
      "Rcoulomb for Ewald summation" },
    { "-tol",     FALSE, etREAL, {&dtol},
      "Tolerance for Ewald summation" }
  };
  FILE        *fp;
  t_inputrec  *ir;
  t_topology  top;
  t_tpxheader tpx;
  t_nrnb      nrnb;
  t_nsborder  *nsb;
  char        title[STRLEN];
  int         natoms,step,status,i,ncg,root;
  real        t,lambda,ewaldcoeff,qtot;
  rvec        *x,*f,*xbuf;
  int         *index;
  bool        bCont;
  real        *charge,*qbuf;
  matrix      box;
  
  /* Start the actual parallel code if necessary */
  cr   = init_par(&argc,&argv);
  root = 0;
  
  if (MASTER(cr)) 
    CopyRight(stderr,argv[0]);
  
  /* Parse command line on all processors, arguments are passed on in 
   * init_par (see above)
   */
  parse_common_args(&argc,argv,
		    PCA_KEEP_ARGS | PCA_NOEXIT_ON_ARGS | PCA_BE_NICE |
		    PCA_CAN_SET_DEFFNM | (MASTER(cr) ? 0 : PCA_QUIET),
		    NFILE,fnm,asize(pa),pa,asize(desc),desc,0,NULL);
  
#ifndef USE_MPI
  if (nnodes > 1) 
    gmx_fatal(FARGS,"GROMACS compiled without MPI support - can't do parallel runs");
#endif
#ifndef USE_THREADS
  if(nthreads > 1)
    gmx_fatal(FARGS,"GROMACS compiled without threads support - can only use one thread");
#endif

  /* Open log files on all processors */
  open_log(ftp2fn(efLOG,NFILE,fnm),cr);
  snew(ir,1);
  
  if (MASTER(cr)) {
    /* Read tpr file etc. */
    read_tpxheader(ftp2fn(efTPX,NFILE,fnm),&tpx,FALSE,NULL,NULL);
    snew(x,tpx.natoms);
    read_tpx(ftp2fn(efTPX,NFILE,fnm),&step,&t,&lambda,ir,
	     box,&natoms,x,NULL,NULL,&top);
    /* Charges */
    qtot = 0;
    snew(charge,natoms);
    for(i=0; (i<natoms); i++) {
      charge[i] = top.atoms.atom[i].q;
      qtot += charge[i];
    }
  
    /* Grid stuff */
    if (opt2parg_bSet("-grid",asize(pa),pa)) {
      ir->nkx = grid[XX];
      ir->nky = grid[YY];
      ir->nkz = grid[ZZ];
    }
    /* Check command line parameters for consistency */
    if ((ir->nkx <= 0) || (ir->nky <= 0) || (ir->nkz <= 0))
      gmx_fatal(FARGS,"PME grid = %d %d %d",ir->nkx,ir->nky,ir->nkz);
    if (opt2parg_bSet("-rc",asize(pa),pa)) 
      ir->rcoulomb = rc;
    if (ir->rcoulomb <= 0)
      gmx_fatal(FARGS,"rcoulomb should be > 0 (not %f)",ir->rcoulomb);
    if (opt2parg_bSet("-order",asize(pa),pa)) 
      ir->pme_order = pme_order;
    if (ir->pme_order <= 0)
      gmx_fatal(FARGS,"pme_order should be > 0 (not %d)",ir->pme_order);
    if (opt2parg_bSet("-tol",asize(pa),pa))
      ir->ewald_rtol = dtol;
    if (ir->ewald_rtol <= 0)
      gmx_fatal(FARGS,"ewald_tol should be > 0 (not %f)",ir->ewald_rtol);
  }
  else {
    init_top(&top);
  }

  /* Add parallellization code here */
  snew(nsb,1);
  if (MASTER(cr)) {
    ncg = top.blocks[ebCGS].multinr[0];
    for(i=0; (i<cr->nnodes-1); i++)
      top.blocks[ebCGS].multinr[i] = min(ncg,(ncg*(i+1))/cr->nnodes);
    for( ; (i<MAXNODES); i++)
      top.blocks[ebCGS].multinr[i] = ncg;
  }
  if (PAR(cr)) {
    /* Distribute the data over processors */
    MPI_Bcast(&natoms,1,MPI_INT,root,MPI_COMM_WORLD);
    /* Set some variables to zero to avoid core dumps */
    ir->opts.ngtc = ir->opts.ngacc = ir->opts.ngfrz = ir->opts.ngener = 0;
    MPI_Bcast(ir,sizeof(*ir),MPI_BYTE,root,MPI_COMM_WORLD);
    MPI_Bcast(&qtot,1,GMX_MPI_REAL,root,MPI_COMM_WORLD);
    /* Call some dedicated communication routines, master sends n-1 times */
    if (MASTER(cr)) {
      for(i=1; (i<cr->nnodes); i++) {
	mv_block(i,&(top.blocks[ebCGS]));
	mv_block(i,&(top.atoms.excl));
      }
    }
    else {
      ld_block(root,&(top.blocks[ebCGS]));
      ld_block(root,&(top.atoms.excl));
    }
    if (!MASTER(cr)) {
      snew(charge,natoms);
      snew(x,natoms);
    }
    MPI_Bcast(charge,natoms,GMX_MPI_REAL,root,MPI_COMM_WORLD);
  }
  ewaldcoeff = calc_ewaldcoeff(ir->rcoulomb,ir->ewald_rtol);
  if (bVerbose)
    pr_inputrec(stdlog,0,"Inputrec",ir);

  /* Allocate memory for temp arrays etc. */
  snew(xbuf,natoms);
  snew(f,natoms);
  snew(qbuf,natoms);
  snew(index,natoms);

  /* Initialize the PME code */  
  init_pme(stdlog,cr,ir->nkx,ir->nky,ir->nkz,ir->pme_order,
	   natoms,bOptFFT,ewald_geometry);
	   
  /* MFlops accounting */
  init_nrnb(&nrnb);
  
  /* Initialize the work division */
  calc_nsb(stdlog,&(top.blocks[ebCGS]),cr->nnodes,nsb,0);
  nsb->nodeid = cr->nodeid;
  print_nsb(stdlog,"pmetest",nsb);  
  
  /* First do PME based on coordinates in tpr file, send them to
   * other processors if needed.
   */
  if (MASTER(cr))
    fprintf(stdlog,"-----\n"
	    "Results based on tpr file %s\n",ftp2fn(efTPX,NFILE,fnm));
  if (PAR(cr)) {
    MPI_Bcast(x[0],natoms*DIM,GMX_MPI_REAL,root,MPI_COMM_WORLD);
    MPI_Bcast(box[0],DIM*DIM,GMX_MPI_REAL,root,MPI_COMM_WORLD);
    MPI_Bcast(&t,1,GMX_MPI_REAL,root,MPI_COMM_WORLD);
  }
  do_my_pme(stdlog,0,bVerbose,ir,x,xbuf,f,charge,qbuf,box,bSort,
	    cr,nsb,&nrnb,ewaldcoeff,&(top.atoms.excl),qtot,index,NULL);

  /* If we have a trajectry file, we will read the frames in it and compute
   * the PME energy.
   */
  if (ftp2bSet(efTRX,NFILE,fnm)) {
    fprintf(stdlog,"-----\n"
	    "Results based on trx file %s\n",ftp2fn(efTRX,NFILE,fnm));
    if (MASTER(cr)) {
      sfree(x);
      natoms = read_first_x(&status,ftp2fn(efTRX,NFILE,fnm),&t,&x,box); 
      if (natoms != top.atoms.nr)
	gmx_fatal(FARGS,"natoms in trx = %d, in tpr = %d",natoms,top.atoms.nr);
      fp = xvgropen(ftp2fn(efXVG,NFILE,fnm),"PME Energy","Time (ps)","E (kJ/mol)");
    }
    else
      fp = NULL;
    do {
      /* Send coordinates, box and time to the other nodes */
      if (PAR(cr)) {
	MPI_Bcast(x[0],natoms*DIM,GMX_MPI_REAL,root,MPI_COMM_WORLD);
	MPI_Bcast(box[0],DIM*DIM,GMX_MPI_REAL,root,MPI_COMM_WORLD);
	MPI_Bcast(&t,1,GMX_MPI_REAL,root,MPI_COMM_WORLD);
      }
      /* Call the PME wrapper function */
      do_my_pme(stdlog,t,bVerbose,ir,x,xbuf,f,charge,qbuf,box,bSort,cr,
		nsb,&nrnb,ewaldcoeff,&(top.atoms.excl),qtot,index,fp);
      /* Only the master processor reads more data */
      if (MASTER(cr))
	bCont = read_next_x(status,&t,natoms,x,box);
      /* Check whether we need to continue */
      if (PAR(cr))
	MPI_Bcast(&bCont,1,MPI_INT,root,MPI_COMM_WORLD);
    } while (bCont);
    
    /* Finish I/O, close files */
    if (MASTER(cr)) {
      close_trx(status);
      fclose(fp);
    }
  }
  
  if (bVerbose) {
    /* Do some final I/O about performance, might be useful in debugging */
    fprintf(stdlog,"-----\n");
    print_nrnb(stdlog,&nrnb);
  }
  
  /* Finish the parallel stuff */  
  if (gmx_parallel_env)
    gmx_finalize(cr);

  /* Thank the audience, as usual */
  if (MASTER(cr)) 
    thanx(stderr);

  return 0;
}
