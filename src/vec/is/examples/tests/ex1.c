#ifndef lint
static char vcid[] = "$Id: ex1.c,v 1.12 1996/03/19 21:22:44 bsmith Exp bsmith $";
#endif

static char help[] = "Tests various IS routines\n\n";

#include "petsc.h"
#include "is.h"
#include "sys.h"
#include <math.h>

int main(int argc,char **argv)
{
  int      n = 5, ierr,indices[5],rank;
  IS       is;

  PetscInitialize(&argc,&argv,(char*)0,help);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  /* create an index set */
  indices[0] = rank + 1; 
  indices[1] = rank + 2; 
  indices[2] = rank + 3; 
  indices[3] = rank + 4; 
  indices[4] = rank + 5; 
  ierr = ISCreateSeq(MPI_COMM_SELF,n,indices,&is); CHKERRA(ierr);

  ierr = ISView(is,VIEWER_STDOUT_SELF); CHKERRA(ierr);
  ierr = ISDestroy(is); CHKERRA(ierr);
  PetscFinalize();
  return 0;
}
 
