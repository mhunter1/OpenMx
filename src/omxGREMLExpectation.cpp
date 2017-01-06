 /*
 *  Copyright 2007-2017 The OpenMx Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
 
#include "omxExpectation.h"
#include "omxFitFunction.h"
#include "omxDefines.h"
#include "omxGREMLExpectation.h"
#include <Eigen/Core>
#include <Eigen/Cholesky>
#include <Eigen/Dense>
 
#ifdef SHADOW_DIAG
#pragma GCC diagnostic warning "-Wshadow"
#endif

void omxInitGREMLExpectation(omxExpectation* ox){
  
  SEXP rObj = ox->rObj;
  SEXP Rmtx, casesToDrop, yXcolnames;
  int i=0;
  omxState* currentState = ox->currentState;
  
  if(OMX_DEBUG) { mxLog("Initializing GREML expectation."); }
  
  //omxGREMLExpectation *oge = (omxGREMLExpectation*) R_alloc(1, sizeof(omxGREMLExpectation));
  omxGREMLExpectation *oge = new omxGREMLExpectation;
  
  /* Set Expectation Calls and Structures */
  ox->computeFun = omxComputeGREMLExpectation;
	ox->destructFun = omxDestroyGREMLExpectation;
	ox->componentFun = omxGetGREMLExpectationComponent;
	ox->populateAttrFun = omxPopulateGREMLAttributes;
	ox->argStruct = (void*) oge;
  
  
    /* Set up expectation structures */
  //y:
  if(OMX_DEBUG) { mxLog("Processing y."); }
  oge->y = new omxData();
  {ScopedProtect p1(Rmtx, R_do_slot(rObj, Rf_install("y")));
	  oge->y->newDataStatic(currentState, Rmtx);
  }
  //V:
	if(OMX_DEBUG) { mxLog("Processing V."); }
	oge->cov = omxNewMatrixFromSlot(rObj, currentState, "V");
  if( oge->cov->rows != oge->cov->cols ){
    Rf_error("'V' matrix is not square");
  }
  //X:
	if(OMX_DEBUG) { mxLog("Processing X."); }
  {ScopedProtect p1(Rmtx, R_do_slot(rObj, Rf_install("X")));
	oge->X = omxNewMatrixFromRPrimitive(Rmtx, currentState, 0, 0);
  }
  //Eigy (local) will have however many rows and 1 column:
  Eigen::Map< Eigen::MatrixXd > Eigy(omxMatrixDataColumnMajor(oge->y->dataMat), oge->y->dataMat->cols, 1);
  if(oge->X->rows != Eigy.rows()){Rf_error("'X' and 'y' matrices have different numbers of rows");}
  //means:
  oge->means = omxInitMatrix(Eigy.rows(), 1, 1, currentState);
  //logdetV_om:
  oge->logdetV_om = omxInitMatrix(1, 1, 1, currentState);
  oge->logdetV_om->data[0] = 0;
  //cholV_fail_om:
  oge->cholV_fail_om = omxInitMatrix(1, 1, 1, currentState);
  oge->cholV_fail_om->data[0] = 0;
  //quadXinv:
  oge->quadXinv.setZero(oge->X->cols, oge->X->cols);
  //original dimensions of V:
  oge->origVdim_om = omxInitMatrix(1, 1, 1, currentState);
  oge->origVdim_om->data[0] = double(oge->cov->rows);


  //Deal with missing data:
  int* casesToDrop_intptr;
  oge->numcases2drop = 0;
  {
  ScopedProtect p1(casesToDrop, R_do_slot(rObj, Rf_install("casesToDrop")));
  if(Rf_length(casesToDrop)){
    if(OMX_DEBUG) { mxLog("Preparing GREML expectation to handle missing data."); }
    oge->numcases2drop = Rf_length(casesToDrop);
    casesToDrop_intptr = INTEGER(casesToDrop);
    oge->dropcase.assign(oge->cov->rows,0);
    for(i=0; i < Rf_length(casesToDrop); i++){
      if(casesToDrop_intptr[i] > oge->cov->rows){
        Rf_warning("casesToDrop vector in GREML expectation contains indices greater than the number of datapoints");
        oge->numcases2drop--; 
      }
      //Need to subtract 1 from the index because R begins array indexing with 1, not 0:
      else{oge->dropcase[casesToDrop_intptr[i]-1] = 1;}
  }}
  }
  if(Eigy.rows() != oge->cov->rows - oge->numcases2drop){
    Rf_error("y and V matrices do not have equal numbers of rows");
  }
  
  
  //column names of y and X:
  {
  ScopedProtect p1(yXcolnames, R_do_slot(rObj, Rf_install("yXcolnames")));
  oge->yXcolnames.resize(Rf_length(yXcolnames));
  for(i=0; i < Rf_length(yXcolnames); i++){
    SEXP elem;
    {ScopedProtect p2(elem, STRING_ELT(yXcolnames, i));
  	oge->yXcolnames[i] = CHAR(elem);}
  }
  }
  
  //Initially compute everything involved in computing means:
  oge->alwaysComputeMeans = 1;
  oge->cholquadX_fail = 0;
  EigenMatrixAdaptor EigX(oge->X);
  Eigen::Map< Eigen::MatrixXd > yhat(omxMatrixDataColumnMajor(oge->means), oge->means->rows, oge->means->cols);
  Eigen::MatrixXd EigV(Eigy.rows(), Eigy.rows());
  Eigen::MatrixXd quadX(oge->X->cols, oge->X->cols);
  //Apparently you need to initialize a matrix's elements before you try to write to its lower triangle:
  quadX.setZero(oge->X->cols, oge->X->cols);
  Eigen::LLT< Eigen::MatrixXd > cholV(Eigy.rows());
  Eigen::LLT< Eigen::MatrixXd > cholquadX(oge->X->cols);
  if( oge->numcases2drop && (oge->cov->rows > Eigy.rows()) ){
    dropCasesAndEigenize(oge->cov, EigV, oge->numcases2drop, oge->dropcase, 1, int(oge->origVdim_om->data[0]));
  }
  else{EigV = Eigen::Map< Eigen::MatrixXd >(omxMatrixDataColumnMajor(oge->cov), oge->cov->rows, oge->cov->cols);}
  //invcov:
  oge->invcov = omxInitMatrix(EigV.rows(), EigV.cols(), 1, currentState);
  Eigen::Map< Eigen::MatrixXd > Vinv(omxMatrixDataColumnMajor(oge->invcov), EigV.rows(), EigV.cols());
  cholV.compute(EigV.selfadjointView<Eigen::Lower>());
  if(cholV.info() != Eigen::Success){
    Rf_error("Expected covariance matrix is non-positive-definite at initial values");
  }
  oge->cholV_vectorD = (( Eigen::MatrixXd )(cholV.matrixL())).diagonal();
  for(i=0; i < oge->X->rows; i++){
    oge->logdetV_om->data[0] += log(oge->cholV_vectorD[i]);
  }
  oge->logdetV_om->data[0] *= 2;
  Vinv = cholV.solve(Eigen::MatrixXd::Identity( EigV.rows(), EigV.cols() )); //<-- V inverse
  oge->XtVinv = EigX.transpose() * Vinv;
  quadX.triangularView<Eigen::Lower>() = oge->XtVinv * EigX;
  cholquadX.compute(quadX.selfadjointView<Eigen::Lower>());
  if(cholquadX.info() != Eigen::Success){
    Rf_error("Cholesky factorization failed at initial values; possibly, the matrix of covariates is rank-deficient");
  }
  oge->cholquadX_vectorD = (( Eigen::MatrixXd )(cholquadX.matrixL())).diagonal();
  oge->quadXinv = ( cholquadX.solve(Eigen::MatrixXd::Identity(oge->X->cols, oge->X->cols)) ).triangularView<Eigen::Lower>();
  yhat = EigX * oge->quadXinv.selfadjointView<Eigen::Lower>() * oge->XtVinv * Eigy;
  
  /*Prepare y as the data that the FIML fitfunction will use:*/
  oge->data2 = ox->data;
  ox->data = oge->y;
  if (oge->data2->hasDefinitionVariables()) {
	  Rf_error("definition variables are incompatible (and unnecessary) with GREML expectation");
  }
}


void omxComputeGREMLExpectation(omxExpectation* ox, FitContext *fc, const char *, const char *) {
  omxGREMLExpectation* oge = (omxGREMLExpectation*) (ox->argStruct);
	omxRecompute(oge->cov, fc);
  int i=0;
  oge->cholV_fail_om->data[0] = 0;
  oge->cholquadX_fail = 0;
  oge->logdetV_om->data[0] = 0;
  
  EigenMatrixAdaptor EigX(oge->X);
  Eigen::Map< Eigen::MatrixXd > Eigy(omxMatrixDataColumnMajor(oge->y->dataMat), oge->y->dataMat->cols, 1);
  Eigen::Map< Eigen::MatrixXd > yhat(omxMatrixDataColumnMajor(oge->means), oge->means->rows, oge->means->cols);
  Eigen::MatrixXd EigV(Eigy.rows(), Eigy.rows());
  Eigen::Map< Eigen::MatrixXd > Vinv(omxMatrixDataColumnMajor(oge->invcov), oge->invcov->rows, oge->invcov->cols);
  Eigen::MatrixXd quadX(oge->X->cols, oge->X->cols);
  quadX.setZero(oge->X->cols, oge->X->cols);
  Eigen::LLT< Eigen::MatrixXd > cholV(oge->y->dataMat->rows);
  Eigen::LLT< Eigen::MatrixXd > cholquadX(oge->X->cols);
  if( oge->numcases2drop && (oge->cov->rows > Eigy.rows()) ){
    dropCasesAndEigenize(oge->cov, EigV, oge->numcases2drop, oge->dropcase, 1, int(oge->origVdim_om->data[0]));
  }
  else{EigV = Eigen::Map< Eigen::MatrixXd >(omxMatrixDataColumnMajor(oge->cov), oge->cov->rows, oge->cov->cols);}
  cholV.compute(EigV.selfadjointView<Eigen::Lower>());
  if(cholV.info() != Eigen::Success){
    oge->cholV_fail_om->data[0] = 1;
    return;
  }
  oge->cholV_vectorD = (( Eigen::MatrixXd )(cholV.matrixL())).diagonal();
  for(i=0; i < oge->X->rows; i++){
    oge->logdetV_om->data[0] += log(oge->cholV_vectorD[i]);
  }
  oge->logdetV_om->data[0] *= 2;
  if(oge->alwaysComputeMeans){
  	Vinv = cholV.solve(Eigen::MatrixXd::Identity( EigV.rows(), EigV.cols() )); //<-- V inverse
  	oge->XtVinv = EigX.transpose() * Vinv;
  }
  /*alwaysComputeMeans is initialized as true, and the only way it can be set to false is by the GREML 
  fitfunction.  If its false, that means that the GREML fitfunction is being used, and it knows how to handle
  a "half-full" Vinv.*/
  else{
  	//V inverse:
  	Vinv.triangularView<Eigen::Lower>() = ( cholV.solve(Eigen::MatrixXd::Identity( EigV.rows(), EigV.cols() )) ).triangularView<Eigen::Lower>();
  	oge->XtVinv = EigX.transpose() * Vinv.selfadjointView<Eigen::Lower>();
  }
  quadX.triangularView<Eigen::Lower>() = oge->XtVinv * EigX;
  cholquadX.compute(quadX.selfadjointView<Eigen::Lower>());
  if(cholquadX.info() != Eigen::Success){ 
    oge->cholquadX_fail = 1;
    return;
  }
  oge->cholquadX_vectorD = (( Eigen::MatrixXd )(cholquadX.matrixL())).diagonal();
  oge->quadXinv = ( cholquadX.solve(Eigen::MatrixXd::Identity(oge->X->cols, oge->X->cols)) ).triangularView<Eigen::Lower>();
  if(oge->alwaysComputeMeans){
    yhat = EigX * oge->quadXinv.selfadjointView<Eigen::Lower>() * oge->XtVinv * Eigy;
  }
}


void omxDestroyGREMLExpectation(omxExpectation* ox) {
	if(OMX_DEBUG) { mxLog("Destroying GREML Expectation."); }
  omxGREMLExpectation* argStruct = (omxGREMLExpectation*)(ox->argStruct);
  ox->data = argStruct->data2;
  omxFreeMatrix(argStruct->means);
  omxFreeMatrix(argStruct->invcov);
  omxFreeMatrix(argStruct->logdetV_om);
  omxFreeMatrix(argStruct->cholV_fail_om);
  omxFreeMatrix(argStruct->origVdim_om);
}


/*Possible TODO: it will require some additional computation, but it is probably best to calculate the final
regression coefficients using QR, which is more numerically stable*/
void omxPopulateGREMLAttributes(omxExpectation *ox, SEXP algebra) {
  if(OMX_DEBUG) { mxLog("Populating GREML expectation attributes."); }

  omxGREMLExpectation* oge = (omxGREMLExpectation*) (ox->argStruct);
  
  Rf_setAttrib(algebra, Rf_install("numStats"), Rf_ScalarReal(oge->y->dataMat->cols));
  Rf_setAttrib(algebra, Rf_install("numFixEff"), Rf_ScalarInteger(oge->X->cols));
  
  Eigen::Map< Eigen::MatrixXd > Eigy(omxMatrixDataColumnMajor(oge->y->dataMat), oge->y->dataMat->cols, 1);
  SEXP b_ext, bcov_ext, yXcolnames;
  oge->quadXinv = oge->quadXinv.selfadjointView<Eigen::Lower>();
  Eigen::MatrixXd GREML_b = oge->quadXinv * oge->XtVinv * Eigy;
  
  {
  ScopedProtect p1(b_ext, Rf_allocMatrix(REALSXP, GREML_b.rows(), 1));
  for(int row = 0; row < GREML_b.rows(); row++){
    REAL(b_ext)[0 * GREML_b.rows() + row] = GREML_b(row,0);
  }
  Rf_setAttrib(algebra, Rf_install("b"), b_ext);
  }
  
  {
  ScopedProtect p1(bcov_ext, Rf_allocMatrix(REALSXP, oge->quadXinv.rows(), 
  	oge->quadXinv.cols()));
  for(int row = 0; row < oge->quadXinv.rows(); row++){
    for(int col = 0; col < oge->quadXinv.cols(); col++){
      REAL(bcov_ext)[col * oge->quadXinv.rows() + row] = oge->quadXinv(row,col);
  }}  
  Rf_setAttrib(algebra, Rf_install("bcov"), bcov_ext);
  }
  
  //yXcolnames:
  {
  ScopedProtect p1(yXcolnames, Rf_allocVector(STRSXP, oge->yXcolnames.size()));
  for(int i=0; i < (int)(oge->yXcolnames.size()); i++){
    SET_STRING_ELT(yXcolnames, i, Rf_mkChar(oge->yXcolnames[i]));
  }
  Rf_setAttrib(algebra, Rf_install("yXcolnames"), yXcolnames);
  }
  
}

omxMatrix* omxGetGREMLExpectationComponent(omxExpectation* ox, const char* component){
/* Return appropriate parts of Expectation to the Fit Function */
  if(OMX_DEBUG) { mxLog("GREML expectation: %s requested--", component); }

	omxGREMLExpectation* oge = (omxGREMLExpectation*)(ox->argStruct);
	omxMatrix* retval = NULL;

	
  if(strEQ("y", component)) {
    retval = oge->y->dataMat;
	}
  else if(strEQ("invcov", component)) {
    retval = oge->invcov;
  }
  else if(strEQ("means", component)) {
  	retval = oge->means;
  }
  else if(strEQ("cholV_fail_om", component)){
    retval = oge->cholV_fail_om;
  }
  else if(strEQ("logdetV_om", component)){
    retval = oge->logdetV_om;
  }
  else if(strEQ("cov", component)) {
		retval = oge->cov;
	} 
  else if(strEQ("X", component)) {
		retval = oge->X;
	}
  else if(strEQ("origVdim_om", component)) {
  	retval = oge->origVdim_om;
  }
  
	if (retval) omxRecompute(retval, NULL);
	
	return retval;
}



static double omxAliasedMatrixElement(omxMatrix *om, int row, int col, int origDim)
{
  int index = 0;
  if(row >= origDim || col >= origDim){
		Rf_error("Requested improper value (%d, %d) from (%d x %d) matrix %s", 
           row + 1, col + 1, origDim, origDim, om->name());
		return (NA_REAL);
	}
	index = col * origDim + row; //<--om should always be column-major by this point.
	return om->data[index];
}



void dropCasesAndEigenize(omxMatrix* om, Eigen::MatrixXd &em, int num2drop, std::vector< int > todrop,
	int symmetric, int origDim){
  
  if(OMX_DEBUG) { mxLog("Trimming out cases with missing data..."); }
  
  if(num2drop < 1){ return; }
  
  omxEnsureColumnMajor(om);

  if(om->algebra == NULL){ //i.e., if omxMatrix is from a frontend MxMatrix
  
    em.setZero(om->rows - num2drop, om->cols - num2drop);
  
    int nextCol = 0;
    int nextRow = 0;
    
    for(int j = 0; j < om->cols; j++) {
  	  if(todrop[j]) continue;
  		nextRow = (symmetric ? nextCol : 0);
  		for(int k = (symmetric ? j : 0); k < om->rows; k++) {
  			if(todrop[k]) continue;
  			em(nextRow,nextCol) = omxAliasedMatrixElement(om, k, j, origDim);
  			nextRow++;
  		}
  		nextCol++;
  	}
  }
  else{ /*If the omxMatrix is from an algebra, then copying is not necessary; it can be resized directly
  and Eigen-mapped, since the algebra will be recalculated back to its original dimensions anyhow.*/
    if(origDim==0){Rf_error("Memory not allocated for algebra %s at downsize time",
       om->name());}
    if(om->rows != origDim || om->cols != origDim){
      //Not sure if there are cases where this should be allowed
      Rf_error("More than one attempt made to downsize algebra %s", om->name());
    	//return;
    }
    
    //int oldRows = om->originalRows;
    //int oldCols = om->originalCols;
    
    int nextCol = 0;
    int nextRow = 0;
    
    om->rows = origDim - num2drop;
    om->cols = origDim - num2drop;
    
    for(int j = 0; j < origDim; j++){ //<--j indexes columns
      if(todrop[j]) continue;
      nextRow = (symmetric ? nextCol : 0);
      for(int k = (symmetric ? j : 0); k < origDim; k++){ //<--k indexes rows
        if(todrop[k]) continue;
        omxSetMatrixElement(om, nextRow, nextCol, omxAliasedMatrixElement(om, k, j, origDim));
        nextRow++;
      }
      nextCol++;
    }
    em = Eigen::Map< Eigen::MatrixXd >(om->data, om->rows, om->cols);
    omxMarkDirty(om); //<--Need to mark it dirty so that it eventually gets recalculated back to original dimensions.
    //^^^Algebras that do not depend upon free parameters, and upon which V does not depend, will not be
    //recalculated back to full size until optimization is complete (the GREML fitfunction is smart about that).
  }
  if(OMX_DEBUG) { mxLog("Finished trimming out cases with missing data..."); }
}



void dropCasesFromAlgdV(omxMatrix* om, int num2drop, std::vector< int > todrop, int symmetric, int origDim){
	
	if(OMX_DEBUG) { mxLog("Trimming out cases with missing data..."); }
	
	if(num2drop < 1 || om->algebra == NULL){ return; }
	
	omxEnsureColumnMajor(om);
	
	if(origDim==0){Rf_error("Memory not allocated for algebra %s at downsize time",
    om->name());}
	if(om->rows != origDim || om->cols != origDim){
		//Not sure if there are cases where this should be allowed
		Rf_error("More than one attempt made to downsize algebra %s", om->name());
		//return;
	}
	
	int nextCol = 0;
	int nextRow = 0;
	
	om->rows = origDim - num2drop;
	om->cols = origDim - num2drop;
	
	for(int j = 0; j < origDim; j++){ //<--j indexes columns
		if(todrop[j]) continue;
		nextRow = (symmetric ? nextCol : 0);
		for(int k = (symmetric ? j : 0); k < origDim; k++){ //<--k indexes rows
			if(todrop[k]) continue;
			omxSetMatrixElement(om, nextRow, nextCol, omxAliasedMatrixElement(om, k, j, origDim));
			nextRow++;
		}
		nextCol++;
	}
	omxMarkDirty(om); //<--Need to mark it dirty so that it eventually gets recalculated back to original dimensions.
	//^^^Algebras that do not depend upon free parameters, and upon which V does not depend, will not be
	//recalculated back to full size until optimization is complete (the GREML fitfunction is smart about that).
	if(OMX_DEBUG) { mxLog("Finished trimming out cases with missing data..."); }
}

