#include "taolinesearch.h"
#include "ipm.h" /*I "ipm.h" I*/

/*
% full-space version assumes
% min   d'*x + (1/2)x'*H*x 
% s.t.       AEz x == bE
%            AIz x >= bI
%
*/
/* 
   x,d in R^n
   f in R
   y in R^mi is slack vector (Ax-b) 
   bin in R^mi (tao->constraints_inequality)
   beq in R^me (tao->constraints_equality)
   lamdai in R^mi (ipmP->lamdai)
   lamdae in R^me (ipmP->lamdae)
   Aeq in R^(me x n) (tao->jacobian_equality)
   Ain in R^(mi x n) (tao->jacobian_inequality)
   H in R^(n x n) (tao->hessian)
   min f=(1/2)*x'*H*x + d'*x   
   s.t.  Aeq*x == beq
         Ain*x >= bin
*/

static PetscErrorCode IPMComputeKKT(TaoSolver tao);
static PetscErrorCode IPMPushInitialPoint(TaoSolver tao);
static PetscErrorCode IPMUpdateK(TaoSolver tao);
static PetscErrorCode IPMGatherRHS(TaoSolver tao,Vec,Vec,Vec,Vec,Vec);
static PetscErrorCode IPMScatterStep(TaoSolver tao,Vec,Vec,Vec,Vec,Vec);


#undef __FUNCT__
#define __FUNCT__ "TaoSolve_IPM"
static PetscErrorCode TaoSolve_IPM(TaoSolver tao)
{
  PetscErrorCode ierr;
  TAO_IPM* ipmP = (TAO_IPM*)tao->data;

  
  TaoSolverTerminationReason reason = TAO_CONTINUE_ITERATING;
  PetscInt iter = 0,its,i;
  PetscReal stepsize=1.0;
  PetscReal step_y,step_l,alpha,tau,sigma,muaff,phi_target;
  PetscFunctionBegin;

  /* Push initial point away from bounds */
  ierr = IPMPushInitialPoint(tao); CHKERRQ(ierr);
  ierr = VecCopy(tao->solution,ipmP->rhs_x); CHKERRQ(ierr);
  ierr = IPMComputeKKT(tao); CHKERRQ(ierr);
  ierr = TaoMonitor(tao,iter++,ipmP->kkt_f,ipmP->phi,0.0,1.0,&reason);


  while (reason == TAO_CONTINUE_ITERATING) {
    ierr = IPMUpdateK(tao); CHKERRQ(ierr);

    /* % compute affine scaling step 
       rhs.lami = -iter.yi.*iter.lami;
       rhs.x = -kkt.rd; 
       rhs.lame = -kkt.rpe; 
       rhs.yi = -kkt.rpi; 
       step = feval(par.compute_step,par,iter,rhs);
       dYaff = step.yi;
       dLaff = step.lami; */
    ierr = VecCopy(tao->solution,ipmP->rhs_x); CHKERRQ(ierr);
    ierr = VecCopy(ipmP->lamdai,ipmP->rhs_lamdai); CHKERRQ(ierr);
    ierr = VecCopy(ipmP->lamdae,ipmP->rhs_lamdae); CHKERRQ(ierr);
    ierr = VecCopy(ipmP->yi,ipmP->rhs_yi); CHKERRQ(ierr);

    ierr = VecPointwiseMult(ipmP->rhs_lamdai,ipmP->lamdai,ipmP->yi); CHKERRQ(ierr);
    ierr = VecScale(ipmP->rhs_lamdai,-1.0); CHKERRQ(ierr);
    ierr = VecCopy(ipmP->rd,ipmP->rhs_x); CHKERRQ(ierr);
    ierr = VecScale(ipmP->rhs_x,-1.0); CHKERRQ(ierr);
    ierr = VecCopy(ipmP->rpe,ipmP->rhs_lamdae); CHKERRQ(ierr);
    ierr = VecScale(ipmP->rhs_lamdae,-1.0); CHKERRQ(ierr);
    ierr = VecCopy(ipmP->rpi,ipmP->rhs_yi); CHKERRQ(ierr);
    ierr = VecScale(ipmP->rhs_yi,-1.0); CHKERRQ(ierr);

    ierr = IPMGatherRHS(tao,ipmP->bigrhs,ipmP->rhs_x,ipmP->rhs_lamdae,
			ipmP->rhs_yi,ipmP->rhs_lamdai); CHKERRQ(ierr);
    ierr = KSPSetOperators(tao->ksp,ipmP->K,ipmP->K,DIFFERENT_NONZERO_PATTERN); CHKERRQ(ierr);
    ierr = KSPSolve(tao->ksp,ipmP->bigrhs,ipmP->bigstep);CHKERRQ(ierr);

    ierr = IPMScatterStep(tao,ipmP->bigstep,tao->stepdirection,ipmP->dlamdae,
			  ipmP->dyi,ipmP->dlamdai); CHKERRQ(ierr);
    ierr = KSPGetIterationNumber(tao->ksp,&its); CHKERRQ(ierr);
    tao->ksp_its += its;
    /* check result */
    //printf("solution = \n");
    //ierr = VecView(ipmP->bigstep,0); CHKERRQ(ierr);
    
    ierr = VecCopy(ipmP->dyi,ipmP->dYaff); CHKERRQ(ierr);
    ierr = VecCopy(ipmP->dlamdai,ipmP->dLaff); CHKERRQ(ierr);

    /* get max stepsizes
       [yaff,step.alp.y] = max_stepsize(iter.yi,step.yi);
       [laff,step.alp.lam] = max_stepsize(iter.lami,step.lami);
       alp = min(step.alp.y,step.alp.lam);
       yaff = iter.yi + alp*step.yi;
       laff = iter.lami + alp*step.lami;
       muaff = yaff'*laff/mi;
    */
     /* Find distance along step direction to closest bound */
    
    ierr = VecStepBoundInfo(ipmP->yi,ipmP->Zero_mi,ipmP->Inf_mi,ipmP->dyi,&step_y,PETSC_NULL,PETSC_NULL); CHKERRQ(ierr);
    ierr = VecStepBoundInfo(ipmP->lamdai,ipmP->Zero_mi,ipmP->Inf_mi,ipmP->dlamdai,&step_l,PETSC_NULL,PETSC_NULL); CHKERRQ(ierr);

    alpha = PetscMin(step_y,step_l);
    alpha = PetscMin(alpha,1.0);
    ipmP->alpha1 = alpha;

    ierr = VecCopy(ipmP->yi,ipmP->Yaff); CHKERRQ(ierr);
    ierr = VecAXPY(ipmP->Yaff,alpha,ipmP->dyi); CHKERRQ(ierr);
    ierr = VecCopy(ipmP->lamdai,ipmP->Laff); CHKERRQ(ierr);
    ierr = VecAXPY(ipmP->Laff,alpha,ipmP->dlamdai); CHKERRQ(ierr);
    ierr = VecDot(ipmP->Yaff,ipmP->Laff,&muaff); CHKERRQ(ierr);
    muaff /= ipmP->mi; CHKERRQ(ierr);

    sigma = (muaff/ipmP->mu);
    sigma *= sigma*sigma;

    /* Compute full step */
    /* rhs.lami = rhs.lami - dYaff.*dLaff + sig*kkt.mu*e); */
    ierr = VecAXPY(ipmP->rhs_lamdai,sigma*ipmP->mu,ipmP->One_mi); CHKERRQ(ierr);
    ierr = VecPointwiseMult(ipmP->dYaff,ipmP->dLaff,ipmP->dYaff); CHKERRQ(ierr);
    ierr = VecAXPY(ipmP->rhs_lamdai, -1.0, ipmP->dYaff); CHKERRQ(ierr);
    /* step = feval(par.compute_step,par,iter,rhs); */
    
    
    ierr = IPMGatherRHS(tao,ipmP->bigrhs,ipmP->rhs_x,ipmP->rhs_lamdae,
			ipmP->rhs_yi,ipmP->rhs_lamdai); CHKERRQ(ierr);
    ierr = PetscObjectStateIncrease((PetscObject)ipmP->K);CHKERRQ(ierr);
    
    ierr = KSPSolve(tao->ksp,ipmP->bigrhs,ipmP->bigstep);CHKERRQ(ierr);
    ierr = KSPGetIterationNumber(tao->ksp,&its); CHKERRQ(ierr);
    tao->ksp_its += its;
    ierr = IPMScatterStep(tao,ipmP->bigstep,tao->stepdirection,ipmP->dlamdae,
			  ipmP->dyi,ipmP->dlamdai); CHKERRQ(ierr);



    /* % get max stepsizes
       tau = min(1,max(par.taumin,1-sig*kkt.mu));
       [y,step.alp.y] = max_stepsize(iter.yi,step.yi);
       [lam,step.alp.lam] = max_stepsize(iter.lami,step.lami);
       alp = min(step.alp.y,step.alp.lam);
       step.alp.y = alp;
       step.alp.lam = alp;
       % apply frac-to-boundary      
       step.alp.y = tau*step.alp.y;
       step.alp.lam = tau*step.alp.lam;
    */

    tau = PetscMax(ipmP->taumin,1.0-sigma*ipmP->mu);
    tau = PetscMin(tau,1.0);
    ierr = VecStepBoundInfo(ipmP->yi,ipmP->Zero_mi,ipmP->Inf_mi,ipmP->dyi,&step_y,PETSC_NULL,PETSC_NULL); CHKERRQ(ierr);
    ierr = VecStepBoundInfo(ipmP->lamdai,ipmP->Zero_mi,ipmP->Inf_mi,ipmP->dlamdai,&step_l,PETSC_NULL,PETSC_NULL); CHKERRQ(ierr);

    alpha = PetscMin(step_y,step_l);
    alpha = PetscMin(alpha,1.0);
    alpha *= tau;
    ipmP->alpha2 = alpha;
    

    /*
    % line-search to achieve sufficient decrease
    fr = 1;
    for i = 1:11
      step.alp.y = fr*step.alp.y;
      step.alp.lam = fr*step.alp.lam;
      iter_trial = update_iter(iter,step);
      kkt_trial = feval(par.eval_kkt,par,iter_trial);
      if kkt_trial.phi <= par.dec*kkt.phi
        break
      else
        fr  = fr*(2^(-i));    <-- TODO: Check if this is right
      end
   end
   iter.ls=i-1;
    */

    phi_target = ipmP->dec * ipmP->phi;
    for (i=0; i<11;i++) {
      ierr = VecAXPY(tao->solution,alpha,tao->stepdirection); CHKERRQ(ierr);
      ierr = VecAXPY(ipmP->lamdae,alpha,ipmP->dlamdae); CHKERRQ(ierr);
      ierr = VecAXPY(ipmP->yi,alpha,ipmP->dyi); CHKERRQ(ierr);
      ierr = VecAXPY(ipmP->lamdai,alpha,ipmP->dlamdai); CHKERRQ(ierr);

      ierr = IPMComputeKKT(tao); CHKERRQ(ierr); CHKERRQ(ierr);
      if (ipmP->phi <= phi_target) break;
      alpha /= 2.0;
    }


    iter++;
    ierr = TaoMonitor(tao,iter,ipmP->kkt_f,ipmP->phi,0.0,stepsize,&reason);
    CHKERRQ(ierr);

  }

  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "TaoSetup_IPM"
static PetscErrorCode TaoSetup_IPM(TaoSolver tao)
{
  TAO_IPM *ipmP = (TAO_IPM*)tao->data;
  PetscInt localmi, localme,i,lo,hi,bigsize;
  PetscScalar one = 1.0, mone = -1.0;
  MPI_Comm comm;
  PetscInt *indices;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  ipmP->mi = ipmP->me = 0;
  comm = ((PetscObject)(tao->solution))->comm;
  ierr = VecGetSize(tao->solution,&ipmP->n); CHKERRQ(ierr);
  if (!tao->gradient) {
    ierr = VecDuplicate(tao->solution, &tao->gradient); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->solution, &tao->stepdirection); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->solution, &ipmP->rd); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->solution, &ipmP->rhs_x); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->solution, &ipmP->work); CHKERRQ(ierr);
  }
  
  if (tao->constraints_inequality) {
    ierr = VecGetSize(tao->constraints_inequality,&ipmP->mi); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->yi); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->lamdai); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->complementarity); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->dyi); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->rhs_yi); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->Yaff); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->dYaff); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->dlamdai); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->rhs_lamdai); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->Laff); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->dLaff); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->rpi); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->Zero_mi); CHKERRQ(ierr);
    ierr = VecSet(ipmP->Zero_mi,0.0); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->One_mi); CHKERRQ(ierr);
    ierr = VecSet(ipmP->One_mi,1.0); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_inequality,&ipmP->Inf_mi); CHKERRQ(ierr);
    ierr = VecSet(ipmP->Inf_mi,TAO_INFINITY); CHKERRQ(ierr);

  }
  if (tao->constraints_equality) {
    ierr = VecGetSize(tao->constraints_equality,&ipmP->me); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_equality,&ipmP->lamdae); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_equality,&ipmP->dlamdae); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_equality,&ipmP->rhs_lamdae); CHKERRQ(ierr);
    ierr = VecDuplicate(tao->constraints_equality,&ipmP->rpe); CHKERRQ(ierr);
  }
  ierr = VecGetLocalSize(ipmP->yi,&localmi); CHKERRQ(ierr);
  ierr = VecGetLocalSize(ipmP->lamdae,&localme); CHKERRQ(ierr);
  bigsize = ipmP->n+2*ipmP->mi+ipmP->me;
    
  ierr = PetscMalloc(bigsize*sizeof(PetscInt),&indices);CHKERRQ(ierr);
  for (i=0;i<ipmP->n;i++) {
    indices[i] = ipmP->n+ipmP->me+ipmP->mi;
  }
  for (i=ipmP->n;i<ipmP->n+ipmP->me;i++) {
    indices[i] = ipmP->n;
  }
  for (i=ipmP->n+ipmP->me;i<ipmP->n+ipmP->mi+ipmP->me;i++) {
    indices[i] = ipmP->n+1;
  }
  for (i=ipmP->n+ipmP->mi+ipmP->me;i<ipmP->n+ipmP->mi+ipmP->me+ipmP->mi;i++) {
    indices[i] = 2;
  }
  ierr = MatCreate(comm,&ipmP->K); CHKERRQ(ierr);
  ierr = MatSetSizes(ipmP->K,PETSC_DECIDE,PETSC_DECIDE,bigsize,bigsize);CHKERRQ(ierr);
  ierr = MatSetType(ipmP->K,MATSEQAIJ); CHKERRQ(ierr);
  ierr = MatSetFromOptions(ipmP->K); CHKERRQ(ierr);

  ierr = MatSeqAIJSetPreallocation(ipmP->K,PETSC_NULL,indices); CHKERRQ(ierr);

  ierr = VecCreate(comm,&ipmP->bigrhs); CHKERRQ(ierr);
  ierr = VecSetSizes(ipmP->bigrhs,PETSC_DECIDE,bigsize); CHKERRQ(ierr);
  ierr = VecSetType(ipmP->bigrhs,VECSEQ); CHKERRQ(ierr);
  ierr = VecDuplicate(ipmP->bigrhs,&ipmP->bigstep); CHKERRQ(ierr);
  ierr = PetscFree(indices);CHKERRQ(ierr);

  PetscFunctionReturn(0);
  
}

#undef __FUNCT__
#define __FUNCT__ "TaoDestroy_IPM"
static PetscErrorCode TaoDestroy_IPM(TaoSolver tao)
{
  TAO_IPM *ipmP = (TAO_IPM*)tao->data;
  PetscErrorCode ierr;
  PetscFunctionBegin;
  ierr = VecDestroy(&ipmP->rd); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->rpe); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->rpi); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->work); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->lamdae); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->lamdai); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->yi); CHKERRQ(ierr);

  ierr = VecDestroy(&ipmP->rhs_x); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->rhs_lamdae); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->rhs_lamdai); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->rhs_yi); CHKERRQ(ierr);

  ierr = VecDestroy(&ipmP->dlamdae); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->dlamdai); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->dyi); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->complementarity); CHKERRQ(ierr);

  ierr = VecDestroy(&ipmP->Yaff); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->dYaff); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->Laff); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->dLaff); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->Zero_mi); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->One_mi); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->Inf_mi); CHKERRQ(ierr);

  ierr = MatDestroy(&ipmP->K); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->bigrhs); CHKERRQ(ierr);
  ierr = VecDestroy(&ipmP->bigstep); CHKERRQ(ierr);

  ierr = PetscFree(tao->data); CHKERRQ(ierr);
  tao->data = PETSC_NULL;
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "TaoSetFromOptions_IPM"
static PetscErrorCode TaoSetFromOptions_IPM(TaoSolver tao)
{
  TAO_IPM *ipmP = (TAO_IPM*)tao->data;
  PetscErrorCode ierr;
  PetscBool flg;
  PetscFunctionBegin;
  ierr = PetscOptionsHead("IPM method for constrained optimization"); CHKERRQ(ierr);
  ierr = PetscOptionsBool("-ipm_monitorkkt","monitor kkt status",PETSC_NULL,ipmP->monitorkkt,&ipmP->monitorkkt,&flg); CHKERRQ(ierr);
  ierr = PetscOptionsReal("-ipm_pushy","parameter to push initial point away from bounds",PETSC_NULL,ipmP->pushy,&ipmP->pushy,&flg);
  ierr = PetscOptionsReal("-ipm_pushlam","parameter to push initial point away from bounds",PETSC_NULL,ipmP->pushlam,&ipmP->pushlam,&flg);
  ierr = PetscOptionsTail(); CHKERRQ(ierr);
  ierr =KSPSetFromOptions(tao->ksp); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "TaoView_IPM"
static PetscErrorCode TaoView_IPM(TaoSolver tao, PetscViewer viewer)
{
  return 0;
}

/* IPMObjectiveAndGradient()
   f = d'x + 0.5 * x' * H * x
   rd = H*x + d + Ae'*lame - Ai'*lami
   rpe = Ae*x - be
   rpi = Ai*x - yi - bi
   mu = yi' * lami/mi;
   com = yi.*lami

   phi = ||rd|| + ||rpe|| + ||rpi|| + ||com||
*/
/*
#undef __FUNCT__
#define __FUNCT__ "IPMObjective"
static PetscErrorCode IPMObjective(TaoLineSearch ls, Vec X, PetscReal *f, void *tptr) 
{
  TaoSolver tao = (TaoSolver)tptr;
  TAO_IPM *ipmP = (TAO_IPM*)tao->data;
  PetscErrorCode ierr;
  PetscFunctionBegin;
  ierr = IPMComputeKKT(tao); CHKERRQ(ierr);
  *f = ipmP->phi;
  PetscFunctionReturn(0);
}
*/

/*
   f = d'x + 0.5 * x' * H * x
   rd = H*x + d + Ae'*lame - Ai'*lami
   rpe = Ae*x - be
   rpi = Ai*x - yi - bi
   mu = yi' * lami/mi;
   com = yi.*lami

   phi = ||rd|| + ||rpe|| + ||rpi|| + ||com||
*/
#undef __FUNCT__
#define __FUNCT__ "IPMComputeKKT"
static PetscErrorCode IPMComputeKKT(TaoSolver tao)
{
  TAO_IPM *ipmP = (TAO_IPM *)tao->data;
  PetscScalar norm;
  PetscErrorCode ierr;
  
  
  ierr = TaoComputeObjectiveAndGradient(tao,tao->solution,&ipmP->kkt_f,ipmP->rd); CHKERRQ(ierr);
  ierr = TaoComputeHessian(tao,tao->solution,&tao->hessian,&tao->hessian_pre,&ipmP->Hflag); CHKERRQ(ierr);
  
  if (ipmP->me > 0) {
    ierr = TaoComputeEqualityConstraints(tao,tao->solution,tao->constraints_equality);
    ierr = TaoComputeJacobianEquality(tao,tao->solution,&tao->jacobian_equality,&tao->jacobian_equality_pre,&ipmP->Aiflag); CHKERRQ(ierr);

    /* rd = rd + Ae'*lamdae */
    ierr = MatMultTranspose(tao->jacobian_equality,ipmP->lamdae,ipmP->work); CHKERRQ(ierr);
    ierr = VecAXPY(ipmP->rd, 1.0, ipmP->work); CHKERRQ(ierr);


    /* rpe = Ae*x - be */
    ierr = MatMult(tao->jacobian_equality,tao->solution,ipmP->rpe);CHKERRQ(ierr);
    ierr = VecAXPY(ipmP->rpe, -1.0, tao->constraints_equality); CHKERRQ(ierr);

  }
  if (ipmP->mi > 0) {
    ierr = TaoComputeInequalityConstraints(tao,tao->solution,tao->constraints_inequality);
    ierr = TaoComputeJacobianInequality(tao,tao->solution,&tao->jacobian_inequality,&tao->jacobian_inequality_pre,&ipmP->Aeflag); CHKERRQ(ierr);

    /* rd = rd + Ai'*lamdai */
    ierr = MatMultTranspose(tao->jacobian_inequality,ipmP->lamdai,ipmP->work); CHKERRQ(ierr);
    ierr = VecAXPY(ipmP->rd, -1.0, ipmP->work); CHKERRQ(ierr);

    /* rpi = Ai*x - yi - bi */
    ierr = MatMult(tao->jacobian_inequality,tao->solution,ipmP->rpi); CHKERRQ(ierr);
    ierr = VecAXPY(ipmP->rpi, -1.0, tao->constraints_inequality); CHKERRQ(ierr);
    ierr = VecAXPY(ipmP->rpi, -1.0, ipmP->yi); CHKERRQ(ierr);

    /* mu = yi'*lami/mi */
    ierr = VecDot(ipmP->yi,ipmP->lamdai,&ipmP->mu); CHKERRQ(ierr);
    ipmP->mu /= ipmP->mi;
    /* com = yi .* lami */
    ierr = VecPointwiseMult(ipmP->complementarity, ipmP->yi,ipmP->lamdai); CHKERRQ(ierr);
  }
  /* phi = ||rd|| + ||rpe|| + ||rpi|| + ||com|| */
  ierr = VecNorm(ipmP->rd,NORM_2,&norm); CHKERRQ(ierr);
  ipmP->phi = norm; 
  if (ipmP->me > 0 ) {
    ierr = VecNorm(ipmP->rpe,NORM_2,&norm); CHKERRQ(ierr);
    ipmP->phi += norm; 
  }
  if (ipmP->mi > 0) {
    ierr = VecNorm(ipmP->rpi,NORM_2,&norm); CHKERRQ(ierr);
    ipmP->phi += norm; 
    ierr = VecNorm(ipmP->complementarity,NORM_2,&norm); CHKERRQ(ierr);
    ipmP->phi += norm; 
  }
  if (ipmP->monitorkkt) {
    ierr = PetscPrintf(PETSC_COMM_WORLD,"obj=%G,\tphi = %G,\tmu=%G\talpha1=%G\talpha2=%G\n",ipmP->kkt_f,ipmP->phi,ipmP->mu,ipmP->alpha1,ipmP->alpha2);
  }
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "IPMFindInitialPoint"
/* Push initial point away from bounds */
PetscErrorCode IPMPushInitialPoint(TaoSolver tao)
{
  TAO_IPM *ipmP = (TAO_IPM *)tao->data;
  PetscErrorCode ierr;

  ierr = TaoComputeVariableBounds(tao); CHKERRQ(ierr);
  if (tao->XL && tao->XU) {
    ierr = VecMedian(tao->XL, tao->solution, tao->XU, tao->solution); CHKERRQ(ierr);
  }
  ierr = VecSet(ipmP->yi,1.0); CHKERRQ(ierr);
  ierr = VecSet(ipmP->lamdai,1.0); CHKERRQ(ierr);
  ierr = VecSet(ipmP->lamdae,1.0); CHKERRQ(ierr);

  ierr = IPMComputeKKT(tao); CHKERRQ(ierr);
  ierr = IPMUpdateK(tao); CHKERRQ(ierr);

  /* Compute affine scaling step */
  ierr = VecCopy(ipmP->rd,ipmP->rhs_x); CHKERRQ(ierr);
  ierr = VecCopy(ipmP->rpe,ipmP->rhs_lamdae); CHKERRQ(ierr);
  ierr = VecCopy(ipmP->rpi,ipmP->rhs_yi); CHKERRQ(ierr);
  ierr = VecCopy(ipmP->complementarity,ipmP->rhs_lamdai); CHKERRQ(ierr);
  ierr = IPMGatherRHS(tao,ipmP->bigrhs,ipmP->rhs_x,ipmP->rhs_lamdae,
		      ipmP->rhs_yi,ipmP->lamdai); CHKERRQ(ierr);
  
  ierr = KSPSetOperators(tao->ksp,ipmP->K,ipmP->K,DIFFERENT_NONZERO_PATTERN); CHKERRQ(ierr);
  ierr = KSPSolve(tao->ksp,ipmP->bigrhs,ipmP->bigstep);CHKERRQ(ierr);
  ierr = IPMScatterStep(tao,ipmP->bigstep,tao->stepdirection,ipmP->dlamdae,
			  ipmP->dyi,ipmP->dlamdai); CHKERRQ(ierr);

  /* Push initial step into feasible region */
  ierr = VecSet(ipmP->rhs_yi,ipmP->pushy); CHKERRQ(ierr);
  ierr = VecAXPY(ipmP->yi,1.0,ipmP->dyi); CHKERRQ(ierr);
  ierr = VecPointwiseMax(ipmP->yi,ipmP->rhs_yi,ipmP->yi); CHKERRQ(ierr);


  ierr = VecSet(ipmP->rhs_lamdai,ipmP->pushlam); CHKERRQ(ierr);
  ierr = VecAXPY(ipmP->lamdai,1.0,ipmP->dlamdai); CHKERRQ(ierr);
  ierr = VecPointwiseMax(ipmP->lamdai,ipmP->rhs_lamdai,ipmP->lamdai); CHKERRQ(ierr);
		 


    /*
  ierr = VecSet(tao->solution,1.0); CHKERRQ(ierr);
  ierr = VecSet(ipmP->yi,1000.0); CHKERRQ(ierr);
  ierr = VecSet(ipmP->lamdae,1.0); CHKERRQ(ierr);
  ierr = VecSet(ipmP->lamdai,1000.0); CHKERRQ(ierr);
    */
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "IPMUpdateK"
/* create K = [ H , Ae', 0, -Ai']; 
              [Ae , 0,   0  , 0];
              [Ai , 0, -Imi ,  0];  
              [ 0 , 0 ,  L,   Y ];  */
PetscErrorCode IPMUpdateK(TaoSolver tao)
{
  TAO_IPM *ipmP = (TAO_IPM *)tao->data;
  PetscErrorCode ierr;
  PetscInt i,j;
  PetscInt ncols,row,newcol,newcols[2];
  const PetscInt *cols;
  const PetscReal *vals;
  PetscReal *l,*y;
  PetscReal *newvals;
  PetscReal minus1=-1.0;
  PetscInt *indices;
  PetscInt subsize;
  PetscFunctionBegin;
  subsize = PetscMax(ipmP->n,ipmP->mi);
  subsize = PetscMax(ipmP->me,subsize);
  subsize = PetscMax(2,subsize);

  ierr = MatZeroEntries(ipmP->K); CHKERRQ(ierr);
  ierr = PetscMalloc(sizeof(PetscInt)*subsize,&indices); CHKERRQ(ierr);
  ierr = PetscMalloc(sizeof(PetscReal)*subsize,&newvals); CHKERRQ(ierr);
  /* Copy H */
  for (i=0;i<ipmP->n;i++) {
    ierr = MatGetRow(tao->hessian,i,&ncols,&cols,&vals); CHKERRQ(ierr);
    ierr = MatSetValues(ipmP->K,1,&i,ncols,cols,vals,INSERT_VALUES); CHKERRQ(ierr);
    ierr = MatRestoreRow(tao->hessian,i,&ncols,&cols,&vals); CHKERRQ(ierr);
  }

  /*      Copy Ae and Ae' */
  for (i=ipmP->n;i<ipmP->me+ipmP->n;i++) {
    ierr = MatGetRow(tao->jacobian_equality,i-ipmP->n,&ncols,&cols,&vals); CHKERRQ(ierr);
    /*Ae*/
    ierr = MatSetValues(ipmP->K,1,&i,ncols,cols,vals,INSERT_VALUES); CHKERRQ(ierr);
    /*Ae'*/
    ierr = MatSetValues(ipmP->K,ncols,cols,1,&i,vals,INSERT_VALUES); CHKERRQ(ierr);
    ierr = MatRestoreRow(tao->jacobian_equality,i-ipmP->n,&ncols,&cols,&vals); CHKERRQ(ierr);
  }

  /* Copy Ai,and Ai' */
  for (i=ipmP->me+ipmP->n;i<ipmP->me+ipmP->n+ipmP->mi;i++) {
    ierr = MatGetRow(tao->jacobian_inequality,i-ipmP->me-ipmP->n,&ncols,&cols,&vals); CHKERRQ(ierr);

    /*Ai*/
    ierr = MatSetValues(ipmP->K,1,&i,ncols,cols,vals,INSERT_VALUES); CHKERRQ(ierr);
    /*-Ai'*/
    for (j=0;j<ncols;j++) {
      newcol = i + ipmP->mi;
      newvals[j] = -vals[j];
    }
    ierr = MatSetValues(ipmP->K,ncols,cols,1,&newcol,newvals,INSERT_VALUES); CHKERRQ(ierr);
    ierr = MatRestoreRow(tao->jacobian_equality,i-ipmP->me-ipmP->n,&ncols,&cols,&vals); CHKERRQ(ierr);
  }
    
  /* -I */
  for (i=ipmP->n+ipmP->me;i<ipmP->n+ipmP->me+ipmP->mi;i++) {
    MatSetValues(ipmP->K,1,&i,1,&i,&minus1,INSERT_VALUES); CHKERRQ(ierr);
  }

  /* Copy L,Y */
  ierr = VecGetArray(ipmP->lamdai,&l); CHKERRQ(ierr);
  ierr = VecGetArray(ipmP->yi,&y); CHKERRQ(ierr);

  for (i=0;i<ipmP->mi;i++) {
    newcols[0] = ipmP->n+ipmP->me+i;
    newcols[1] = ipmP->n+ipmP->me+ipmP->mi+i;
    newvals[0] = l[i];
    newvals[1] = y[i];
    row = i + ipmP->n+ipmP->me+ipmP->mi;
    ierr = MatSetValues(ipmP->K,1,&row,2,newcols,newvals,INSERT_VALUES); CHKERRQ(ierr);
  }
      
  ierr = VecRestoreArray(ipmP->lamdai,&l); CHKERRQ(ierr);
  ierr = VecRestoreArray(ipmP->yi,&y); CHKERRQ(ierr);
      
  ierr = PetscFree(indices); CHKERRQ(ierr);
  ierr = PetscFree(newvals); CHKERRQ(ierr);
  ierr = MatAssemblyBegin(ipmP->K,MAT_FINAL_ASSEMBLY);
  ierr = MatAssemblyEnd(ipmP->K,MAT_FINAL_ASSEMBLY);

  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "IPMGatherRHS"
PetscErrorCode IPMGatherRHS(TaoSolver tao,Vec RHS,Vec X,Vec LE,Vec YI,Vec LI)
{
  TAO_IPM *ipmP = (TAO_IPM *)tao->data;
  PetscScalar *x,*rhs;
  PetscInt i;
  PetscErrorCode ierr;
  PetscFunctionBegin;
  /* rhs = [x
            lamdae
	    yi
	    lamdai] */
  ierr = VecGetArray(RHS,&rhs); CHKERRQ(ierr);
  /*x*/
  ierr = VecGetArray(X,&x); CHKERRQ(ierr);
  for (i=0;i<ipmP->n;i++) {
    rhs[i] = x[i];
  }
  ierr = VecRestoreArray(X,&x); CHKERRQ(ierr);

  /*lamdae*/
  ierr = VecGetArray(LE,&x); CHKERRQ(ierr);
  for (i=0;i<ipmP->me;i++) {
    rhs[i+ipmP->n] = x[i];
  }
  ierr = VecRestoreArray(LE,&x); CHKERRQ(ierr);

  /*yi*/
  ierr = VecGetArray(YI,&x); CHKERRQ(ierr);
  for (i=0;i<ipmP->mi;i++) {
    rhs[i+ipmP->n+ipmP->me] = x[i];
  }
  ierr = VecRestoreArray(YI,&x); CHKERRQ(ierr);

  /*lamdai*/
  ierr = VecGetArray(LI,&x); CHKERRQ(ierr);
  for (i=0;i<ipmP->mi;i++) {
    rhs[i+ipmP->n+ipmP->me+ipmP->mi] = x[i];
  }
  ierr = VecRestoreArray(LI,&x); CHKERRQ(ierr);
  ierr = VecRestoreArray(RHS,&rhs); CHKERRQ(ierr);

  PetscFunctionReturn(0);
}



#undef __FUNCT__
#define __FUNCT__ "IPMScatterStep"
PetscErrorCode IPMScatterStep(TaoSolver tao, Vec STEP, Vec X, Vec LE, Vec YI, Vec LI)
{
  TAO_IPM *ipmP = (TAO_IPM *)tao->data;
  PetscScalar *x,*step;
  PetscInt i;
  PetscErrorCode ierr;
  PetscFunctionBegin;
  /*        [x
            lamdae   = step
 	    yi
	    lamdai] */
  ierr = VecGetArray(STEP,&step); CHKERRQ(ierr);
  /*x*/
  ierr = VecGetArray(X,&x); CHKERRQ(ierr);
  for (i=0;i<ipmP->n;i++) {
    x[i] = step[i];
  }
  ierr = VecRestoreArray(X,&x); CHKERRQ(ierr);

  /*lamdae*/
  ierr = VecGetArray(LE,&x); CHKERRQ(ierr);
  for (i=0;i<ipmP->me;i++) {
    x[i] = step[i+ipmP->n];
  }
  ierr = VecRestoreArray(LE,&x); CHKERRQ(ierr);

  /*yi*/
  ierr = VecGetArray(YI,&x); CHKERRQ(ierr);
  for (i=0;i<ipmP->mi;i++) {
    x[i] = step[i+ipmP->n+ipmP->me];
  }
  ierr = VecRestoreArray(YI,&x); CHKERRQ(ierr);

  /*lamdai*/
  ierr = VecGetArray(LI,&x); CHKERRQ(ierr);
  for (i=0;i<ipmP->mi;i++) {
    x[i] = step[i+ipmP->n+ipmP->me+ipmP->mi];
  }
  ierr = VecRestoreArray(LI,&x); CHKERRQ(ierr);
  ierr = VecRestoreArray(STEP,&step); CHKERRQ(ierr);

  PetscFunctionReturn(0);
}


EXTERN_C_BEGIN

#undef __FUNCT__
#define __FUNCT__ "TaoCreate_IPM"
PetscErrorCode TaoCreate_IPM(TaoSolver tao)
{
  TAO_IPM *ipmP;
  //  const char *ipmls_type = TAOLINESEARCH_IPM;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  tao->ops->setup = TaoSetup_IPM;
  tao->ops->solve = TaoSolve_IPM;
  tao->ops->view = TaoView_IPM;
  tao->ops->setfromoptions = TaoSetFromOptions_IPM;
  tao->ops->destroy = TaoDestroy_IPM;
  //tao->ops->computedual = TaoComputeDual_IPM;

  ierr = PetscNewLog(tao, TAO_IPM, &ipmP); CHKERRQ(ierr);
  tao->data = (void*)ipmP;
  tao->max_it = 2000;
  tao->max_funcs = 4000;
  tao->fatol = 1e-4;
  tao->frtol = 1e-4;
  ipmP->dec = 10000; /* line search critera */
  ipmP->taumin = 0.995;
  ipmP->monitorkkt = PETSC_FALSE;
  ipmP->pushy = 1000;
  ipmP->pushlam = 1000;
  /*
  ierr = TaoLineSearchCreate(((PetscObject)tao)->comm, &tao->linesearch); CHKERRQ(ierr);
  ierr = TaoLineSearchSetType(tao->linesearch, ipmls_type); CHKERRQ(ierr);
  ierr = TaoLineSearchSetObjectiveRoutine(tao->linesearch, IPMObjective, tao); CHKERRQ(ierr);
  */
  ierr = KSPCreate(((PetscObject)tao)->comm, &tao->ksp); CHKERRQ(ierr);
  PetscFunctionReturn(0);
  

}
EXTERN_C_END

