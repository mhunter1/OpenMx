/*
 *  Copyright 2007-2017 The OpenMx Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "omxExpectation.h"
#include "omxFitFunction.h"
#include "omxDefines.h"
#include "omxNormalExpectation.h"

#ifdef SHADOW_DIAG
#pragma GCC diagnostic warning "-Wshadow"
#endif

void omxComputeNormalExpectation(omxExpectation* ox, FitContext *fc, const char *, const char *) {
	omxNormalExpectation* one = (omxNormalExpectation*) (ox->argStruct);

	omxRecompute(one->cov, fc);
	if(one->means != NULL)
	    omxRecompute(one->means, fc);
	if (one->thresholds) omxRecompute(one->thresholds, fc);
}

void omxDestroyNormalExpectation(omxExpectation* ox) {

	if(OMX_DEBUG) { mxLog("Destroying Normal Expectation."); }

}

void omxPopulateNormalAttributes(omxExpectation *ox, SEXP algebra) {
    if(OMX_DEBUG) { mxLog("Populating Normal Attributes."); }

	omxNormalExpectation* one = (omxNormalExpectation*) (ox->argStruct);
    
	omxMatrix *cov = one->cov;
	omxMatrix *means = one->means;

	omxRecompute(cov, NULL);
	if(means != NULL) omxRecompute(means, NULL);

	{
		SEXP expCovExt;
	ScopedProtect p1(expCovExt, Rf_allocMatrix(REALSXP, cov->rows, cov->cols));
	for(int row = 0; row < cov->rows; row++)
		for(int col = 0; col < cov->cols; col++)
			REAL(expCovExt)[col * cov->rows + row] =
				omxMatrixElement(cov, row, col);
	Rf_setAttrib(algebra, Rf_install("ExpCov"), expCovExt);
	}

	
	if (means != NULL) {
		SEXP expMeanExt;
		ScopedProtect p1(expMeanExt, Rf_allocMatrix(REALSXP, means->rows, means->cols));
		for(int row = 0; row < means->rows; row++)
			for(int col = 0; col < means->cols; col++)
				REAL(expMeanExt)[col * means->rows + row] =
					omxMatrixElement(means, row, col);
		Rf_setAttrib(algebra, Rf_install("ExpMean"), expMeanExt);
	} else {
		SEXP expMeanExt;
		ScopedProtect p1(expMeanExt, Rf_allocMatrix(REALSXP, 0, 0));
		Rf_setAttrib(algebra, Rf_install("ExpMean"), expMeanExt);
	}

	Rf_setAttrib(algebra, Rf_install("numStats"), Rf_ScalarReal(omxDataDF(ox->data)));
}

void omxInitNormalExpectation(omxExpectation* ox) {
	
	SEXP rObj = ox->rObj;
	omxState* currentState = ox->currentState;

    if(OMX_DEBUG) { mxLog("Initializing Normal expectation."); }

	omxNormalExpectation *one = (omxNormalExpectation*) R_alloc(1, sizeof(omxNormalExpectation));
	
	/* Set Expectation Calls and Structures */
	ox->computeFun = omxComputeNormalExpectation;
	ox->destructFun = omxDestroyNormalExpectation;
	ox->componentFun = omxGetNormalExpectationComponent;
	ox->populateAttrFun = omxPopulateNormalAttributes;
	ox->argStruct = (void*) one;
	
	/* Set up expectation structures */
	if(OMX_DEBUG) { mxLog("Processing cov."); }
	one->cov = omxNewMatrixFromSlot(rObj, currentState, "covariance");

	if(OMX_DEBUG) { mxLog("Processing Means."); }
	one->means = omxNewMatrixFromSlot(rObj, currentState, "means");

	one->thresholds = omxNewMatrixFromSlot(rObj, currentState, "thresholds");
}

omxMatrix* omxGetNormalExpectationComponent(omxExpectation* ox, const char* component){
/* Return appropriate parts of Expectation to the Fit Function */
	if(OMX_DEBUG) { mxLog("Normal expectation: %s requested--", component); }

	omxNormalExpectation* one = (omxNormalExpectation*)(ox->argStruct);
	omxMatrix* retval = NULL;

	if(strEQ("cov", component)) {
		retval = one->cov;
	} else if(strEQ("means", component)) {
		retval = one->means;
	} else if(strEQ("pvec", component)) {
		// Once implemented, change compute function and return pvec
	}
	if (retval) omxRecompute(retval, NULL);
	
	return retval;
}
