#ifndef lint
static char vcid[] = "$Id: ex9.c,v 1.31 1996/03/19 21:23:15 bsmith Exp bsmith $";
#endif

static char help[]= "Scatters from a parallel vector to a sequential vector.\n\n";

#include "petsc.h"
#include "is.h"
#include "vec.h"
#include "sys.h"
#include <math.h>

int main(int argc,char **argv)
{
  int           n = 5, ierr, idx2[3] = {0,2,3}, idx1[3] = {0,1,2};
  int           size,rank,i;
  Scalar        mone = -1.0, value;
  Vec           x,y;
  IS            is1,is2;
  VecScatter    ctx = 0;

  PetscInitialize(&argc,&argv,(char*)0,help); 
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  /* create two vectors */
  ierr = VecCreateMPI(MPI_COMM_WORLD,PETSC_DECIDE,size*n,&x); CHKERRA(ierr);
  ierr = VecCreateSeq(MPI_COMM_SELF,n,&y); CHKERRA(ierr);

  /* create two index sets */
  ierr = ISCreateSeq(MPI_COMM_SELF,3,idx1,&is1); CHKERRA(ierr);
  ierr = ISCreateSeq(MPI_COMM_SELF,3,idx2,&is2); CHKERRA(ierr);

  /* fill local part of parallel vector */
  for ( i=n*rank; i<n*(rank+1); i++ ) {
    value = (Scalar) i;
    ierr = VecSetValues(x,1,&i,&value,INSERT_VALUES); CHKERRA(ierr);
  }
  ierr = VecAssemblyBegin(x); CHKERRA(ierr);
  ierr = VecAssemblyEnd(x); CHKERRA(ierr);

  ierr = VecView(x,VIEWER_STDOUT_WORLD); CHKERRA(ierr);

  ierr = VecSet(&mone,y); CHKERRA(ierr);

  ierr = VecScatterCreate(x,is1,y,is2,&ctx); CHKERRA(ierr);
  ierr = VecScatterBegin(x,y,INSERT_VALUES,SCATTER_ALL,ctx);CHKERRA(ierr);
  ierr = VecScatterEnd(x,y,INSERT_VALUES,SCATTER_ALL,ctx); CHKERRA(ierr);
  ierr = VecScatterDestroy(ctx); CHKERRA(ierr);

  if (!rank) {
    PetscPrintf(MPI_COMM_SELF,"scattered vector\n"); 
    ierr = VecView(y,VIEWER_STDOUT_SELF); CHKERRA(ierr);
  }
  ierr = ISDestroy(is1); CHKERRA(ierr);
  ierr = ISDestroy(is2); CHKERRA(ierr);
  ierr = VecDestroy(x); CHKERRA(ierr);
  ierr = VecDestroy(y); CHKERRA(ierr);

  PetscFinalize(); 
  return 0;
}
 
