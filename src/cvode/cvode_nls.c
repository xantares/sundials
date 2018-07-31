/* -----------------------------------------------------------------------------
 * Programmer(s): David J. Gardner @ LLNL
 * -----------------------------------------------------------------------------
 * LLNS Copyright Start
 * Copyright (c) 2014, Lawrence Livermore National Security
 * This work was performed under the auspices of the U.S. Department
 * of Energy by Lawrence Livermore National Laboratory in part under
 * Contract W-7405-Eng-48 and in part under Contract DE-AC52-07NA27344.
 * Produced at the Lawrence Livermore National Laboratory.
 * All rights reserved.
 * For details, see the LICENSE file.
 * LLNS Copyright End
 * -----------------------------------------------------------------------------
 * This the implementation file for the CVODE nonlinear solver interface.
 * ---------------------------------------------------------------------------*/

#include "cvode_impl.h"
#include "sundials/sundials_math.h"

/* constant macros */
#define ONE RCONST(1.0) /* real 1.0 */

/* nonlinear solver constants
     NLS_MAXCOR  maximum no. of corrector iterations for the nonlinear solver
     CRDOWN      constant used in the estimation of the convergence rate (crate)
                 of the iterates for the nonlinear equation
     RDIV        declare divergence if ratio del/delp > RDIV
 */
#define NLS_MAXCOR 3
#define CRDOWN     RCONST(0.3)
#define RDIV       RCONST(2.0)

/* private functions */
static int cvNlsResidual(N_Vector ycor, N_Vector res, void* cvode_mem);
static int cvNlsFPFunction(N_Vector ycor, N_Vector res, void* cvode_mem);

static int cvNlsLSetup(N_Vector ycor, N_Vector res, booleantype jbad,
                       booleantype* jcur, void* cvode_mem);
static int cvNlsLSolve(N_Vector ycor, N_Vector delta, void* cvode_mem);
static int cvNlsConvTest(SUNNonlinearSolver NLS, N_Vector ycor, N_Vector del,
                         realtype tol, N_Vector ewt, void* cvode_mem);

/* -----------------------------------------------------------------------------
 * Exported functions
 * ---------------------------------------------------------------------------*/

int CVodeSetNonlinearSolver(void *cvode_mem, SUNNonlinearSolver NLS)
{
  CVodeMem cv_mem;
  int retval;

  /* Return immediately if CVode memory is NULL */
  if (cvode_mem == NULL) {
    cvProcessError(NULL, CV_MEM_NULL, "CVODE", "CVodeSetNonlinearSolver", MSGCV_NO_MEM);
    return(CV_MEM_NULL);
  }
  cv_mem = (CVodeMem) cvode_mem;

  /* Return immediately if NLS memory is NULL */
  if (NLS == NULL) {
    cvProcessError(NULL, CV_ILL_INPUT, "CVODE", "CVodeSetNonlinearSolver",
                   "NLS must be non-NULL");
    return (CV_ILL_INPUT);
  }

  /* check for required nonlinear solver functions */
  if ( NLS->ops->gettype    == NULL ||
       NLS->ops->initialize == NULL ||
       NLS->ops->solve      == NULL ||
       NLS->ops->free       == NULL ||
       NLS->ops->setsysfn   == NULL ) {
    cvProcessError(cv_mem, CV_ILL_INPUT, "CVODE", "CVodeSetNonlinearSolver",
                   "NLS does not support required operations");
    return(CV_ILL_INPUT);
  }

  /* free any existing nonlinear solver */
  if (cv_mem->NLS) retval = SUNNonlinSolFree(cv_mem->NLS);

  /* set SUNNonlinearSolver pointer */
  cv_mem->NLS = NLS;

  /* set the nonlinear system function */
  if (SUNNonlinSolGetType(NLS) == SUNNONLINEARSOLVER_ROOTFIND) {
    retval = SUNNonlinSolSetSysFn(cv_mem->NLS, cvNlsResidual);
  } else if (SUNNonlinSolGetType(NLS) ==  SUNNONLINEARSOLVER_FIXEDPOINT) {
    retval = SUNNonlinSolSetSysFn(cv_mem->NLS, cvNlsFPFunction);
  } else {
    cvProcessError(cv_mem, CV_ILL_INPUT, "CVODE", "CVodeSetNonlinearSolver",
                   "Invalid nonlinear solver type");
    return(CV_ILL_INPUT);
  }

  if (retval != CV_SUCCESS) {
    cvProcessError(cv_mem, CV_ILL_INPUT, "CVODE", "CVodeSetNonlinearSolver",
                   "Setting nonlinear system function failed");
    return(CV_ILL_INPUT);
  }

  /* set convergence test function */
  retval = SUNNonlinSolSetConvTestFn(cv_mem->NLS, cvNlsConvTest);
  if (retval != CV_SUCCESS) {
    cvProcessError(cv_mem, CV_ILL_INPUT, "CVODE", "CVodeSetNonlinearSolver",
                   "Setting convergence test function failed");
    return(CV_ILL_INPUT);
  }

  /* set max allowed nonlinear iterations */
  retval = SUNNonlinSolSetMaxIters(cv_mem->NLS, NLS_MAXCOR);
  if (retval != CV_SUCCESS) {
    cvProcessError(cv_mem, CV_ILL_INPUT, "CVODE", "CVodeSetNonlinearSolver",
                   "Setting maximum number of nonlinear iterations failed");
    return(CV_ILL_INPUT);
  }

  return(CV_SUCCESS);
}


/* -----------------------------------------------------------------------------
 * Private functions
 * ---------------------------------------------------------------------------*/


int cvNlsInit(CVodeMem cvode_mem)
{
  int retval;

  /* set the linear solver setup wrapper function */
  if (cvode_mem->cv_lsetup)
    retval = SUNNonlinSolSetLSetupFn(cvode_mem->NLS, cvNlsLSetup);
  else
    retval = SUNNonlinSolSetLSetupFn(cvode_mem->NLS, NULL);

  if (retval != CV_SUCCESS) {
    cvProcessError(cvode_mem, CV_ILL_INPUT, "CVODE", "cvInitialSetup",
                   "Setting the linear solver setup function failed");
    return(CV_NLS_INIT_FAIL);
  }

  /* set the linear solver solve wrapper function */
  if (cvode_mem->cv_lsolve)
    retval = SUNNonlinSolSetLSolveFn(cvode_mem->NLS, cvNlsLSolve);
  else
    retval = SUNNonlinSolSetLSolveFn(cvode_mem->NLS, NULL);

  if (retval != CV_SUCCESS) {
    cvProcessError(cvode_mem, CV_ILL_INPUT, "CVODE", "cvInitialSetup",
                   "Setting linear solver solve function failed");
    return(CV_NLS_INIT_FAIL);
  }

  /* initialize nonlinear solver */
  retval = SUNNonlinSolInitialize(cvode_mem->NLS);

  if (retval != CV_SUCCESS) {
    cvProcessError(cvode_mem, CV_ILL_INPUT, "CVODE", "cvInitialSetup",
                   MSGCV_NLS_INIT_FAIL);
    return(CV_NLS_INIT_FAIL);
  }

  return(CV_SUCCESS);
}


static int cvNlsLSetup(N_Vector ycor, N_Vector res, booleantype jbad,
                       booleantype* jcur, void* cvode_mem)
{
  CVodeMem cv_mem;
  int      retval;

  if (cvode_mem == NULL) {
    cvProcessError(NULL, CV_MEM_NULL, "CVODE", "cvNlsLSetup", MSGCV_NO_MEM);
    return(CV_MEM_NULL);
  }
  cv_mem = (CVodeMem) cvode_mem;

  /* if the nonlinear solver marked the Jacobian as bad update convfail */
  if (jbad)
    cv_mem->convfail = CV_FAIL_BAD_J;

  /* setup the linear solver */
  retval = cv_mem->cv_lsetup(cv_mem, cv_mem->convfail, cv_mem->cv_y, cv_mem->cv_ftemp,
                             &(cv_mem->cv_jcur), cv_mem->cv_vtemp1, cv_mem->cv_vtemp2,
                             cv_mem->cv_vtemp3);
  cv_mem->cv_nsetups++;

  /* update Jacobian status */
  *jcur = cv_mem->cv_jcur;

  cv_mem->cv_gamrat = cv_mem->cv_crate = ONE;
  cv_mem->cv_gammap = cv_mem->cv_gamma;
  cv_mem->cv_nstlp  = cv_mem->cv_nst;

  if (retval < 0) return(CV_LSETUP_FAIL);
  if (retval > 0) return(SUN_NLS_CONV_RECVR);

  return(CV_SUCCESS);
}


static int cvNlsLSolve(N_Vector ycor, N_Vector delta, void* cvode_mem)
{
  CVodeMem cv_mem;
  int      retval;

  if (cvode_mem == NULL) {
    cvProcessError(NULL, CV_MEM_NULL, "CVODE", "cvNlsLSolve", MSGCV_NO_MEM);
    return(CV_MEM_NULL);
  }
  cv_mem = (CVodeMem) cvode_mem;

  retval = cv_mem->cv_lsolve(cv_mem, delta, cv_mem->cv_ewt, cv_mem->cv_y, cv_mem->cv_ftemp);

  if (retval < 0) return(CV_LSOLVE_FAIL);
  if (retval > 0) return(SUN_NLS_CONV_RECVR);

  return(CV_SUCCESS);
}


static int cvNlsConvTest(SUNNonlinearSolver NLS, N_Vector ycor, N_Vector delta,
                         realtype tol, N_Vector ewt, void* cvode_mem)
{
  CVodeMem cv_mem;
  int m, retval;
  realtype del;
  realtype dcon;
  static realtype delp;

  if (cvode_mem == NULL) {
    cvProcessError(NULL, CV_MEM_NULL, "CVODE", "cvNlsConvTest", MSGCV_NO_MEM);
    return(CV_MEM_NULL);
  }
  cv_mem = (CVodeMem) cvode_mem;

  /* compute the norm of the correction */
  del = N_VWrmsNorm(delta, ewt);

  /* get the current nonlinear solver iteration count */
  retval = SUNNonlinSolGetCurIter(NLS, &m);
  if (retval != CV_SUCCESS) return(CV_MEM_NULL);

  /* Test for convergence. If m > 0, an estimate of the convergence
     rate constant is stored in crate, and used in the test.        */
  if (m > 0) {
    cv_mem->cv_crate = SUNMAX(CRDOWN * cv_mem->cv_crate, del/delp);
  }
  dcon = del * SUNMIN(ONE, cv_mem->cv_crate) / tol;

  if (dcon <= ONE) {
    cv_mem->cv_acnrm = (m==0) ? del : N_VWrmsNorm(ycor, cv_mem->cv_ewt);
    return(CV_SUCCESS); /* Nonlinear system was solved successfully */
  }

  /* check if the iteration seems to be diverging */
  if ((m >= 1) && (del > RDIV*delp)) return(SUN_NLS_CONV_RECVR);

  /* Save norm of correction and loop again */
  delp = del;

  /* Not yet converged */
  return(SUN_NLS_CONTINUE);
}


static int cvNlsResidual(N_Vector ycor, N_Vector res, void* cvode_mem)
{
  CVodeMem cv_mem;
  int retval;

  if (cvode_mem == NULL) {
    cvProcessError(NULL, CV_MEM_NULL, "CVODE", "cvNlsResidual", MSGCV_NO_MEM);
    return(CV_MEM_NULL);
  }
  cv_mem = (CVodeMem) cvode_mem;

  /* update the state based on the current correction */
  N_VLinearSum(ONE, cv_mem->cv_zn[0], ONE, ycor, cv_mem->cv_y);

  /* evaluate the rhs function */
  retval = cv_mem->cv_f(cv_mem->cv_tn, cv_mem->cv_y, cv_mem->cv_ftemp,
                        cv_mem->cv_user_data);
  cv_mem->cv_nfe++;
  if (retval < 0) return(CV_RHSFUNC_FAIL);
  if (retval > 0) return(RHSFUNC_RECVR);

  N_VLinearSum(cv_mem->cv_rl1, cv_mem->cv_zn[1], ONE, ycor, res);
  N_VLinearSum(-cv_mem->cv_gamma, cv_mem->cv_ftemp, ONE, res, res);

  return(CV_SUCCESS);
}


static int cvNlsFPFunction(N_Vector ycor, N_Vector res, void* cvode_mem)
{
 CVodeMem cv_mem;
  int retval;

  if (cvode_mem == NULL) {
    cvProcessError(NULL, CV_MEM_NULL, "CVODE", "cvNlsFPFunction", MSGCV_NO_MEM);
    return(CV_MEM_NULL);
  }
  cv_mem = (CVodeMem) cvode_mem;

  /* update the state based on the current correction */
  N_VLinearSum(ONE, cv_mem->cv_zn[0], ONE, ycor, cv_mem->cv_y);

  /* evaluate the rhs function */
  retval = cv_mem->cv_f(cv_mem->cv_tn, cv_mem->cv_y, res,
                        cv_mem->cv_user_data);
  cv_mem->cv_nfe++;
  if (retval < 0) return(CV_RHSFUNC_FAIL);
  if (retval > 0) return(RHSFUNC_RECVR);

  N_VLinearSum(cv_mem->cv_h, res, -ONE, cv_mem->cv_zn[1], res);
  N_VScale(cv_mem->cv_rl1, res, res);

  return(CV_SUCCESS);
}