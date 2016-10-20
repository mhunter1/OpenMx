/*
 *  Copyright 2007-2016 The OpenMx Project
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

/***********************************************************
* 
*  omxFitFunction.h
*
*  Created: Timothy R. Brick 	Date: 2009-02-17
*
*	Contains header information for the omxFitFunction class
*   omxFitFunction objects hold necessary information to simplify
* 	fit function calculation.
*
**********************************************************/

#ifndef _OMXFITFUNCTION_H_
#define _OMXFITFUNCTION_H_

#include "omxDefines.h"
#include <R_ext/Rdynload.h> 
#include <R_ext/BLAS.h>
#include <R_ext/Lapack.h>

#include "omxMatrix.h"
#include "omxAlgebra.h"
#include "omxData.h"
#include "omxState.h"
#include "omxExpectation.h"
#include "Compute.h"

typedef struct {
	char label[250];
	double* values;
	int numValues;
	int rows, cols;
} omxRListElement;

enum FitStatisticUnits {
	FIT_UNITS_UNINITIALIZED=0,
	FIT_UNITS_UNKNOWN,
	FIT_UNITS_MINUS2LL,
	FIT_UNITS_SQUARED_RESIDUAL  // OK?
};

struct omxFitFunction {					// A fit function

	/* Fields unique to FitFunction Functions */
	void (*initFun)(omxFitFunction *oo);
	void (*destructFun)(omxFitFunction* oo);									// Wrapper for the destructor object

	void (*computeFun)(omxFitFunction* oo, int ffcompute, FitContext *fc);
	void (*ciFun)(omxFitFunction* oo, int ffcompute, FitContext *fc);

	omxRListElement* (*setFinalReturns)(omxFitFunction* oo, int *numVals); // DEPRECATED, use addOutput instead

	// addOutput should only be used for returning global results
	void (*addOutput)(omxFitFunction* oo, MxRList *out);

	// populateAttrFun should be used for returning results specific to fit functions or expectations
	void (*populateAttrFun)(omxFitFunction* oo, SEXP algebra);

	void (*setVarGroup)(omxFitFunction*, FreeVarGroup *);
	
	SEXP rObj;																	// Original r Object Pointer
	omxExpectation* expectation;												// Data expectation object
	void* argStruct;															// Arguments needed for fit function
// This is always a pointer to a static string.
// We do not need to allocate or free it.
	const char* fitType;														// Type of FitFunction Function

	omxMatrix* matrix;
	bool initialized;
	FreeVarGroup *freeVarGroup;
	bool gradientAvailable;
	bool hessianAvailable;
	FitStatisticUnits units;
	bool canDuplicate;
	bool openmpUser; // can decide this in preoptimize

	void setUnitsFromName(const char *name);
	const char *name() const { return matrix->name(); }
};

/* Initialize and Destroy */
omxFitFunction *omxNewInternalFitFunction(omxState* os, const char *fitType,
					  omxExpectation *expect, omxMatrix *matrix, bool rowLik);
void omxFillMatrixFromMxFitFunction(omxMatrix* om, const char *fitType, int matrixNumber, SEXP rObj);
void omxChangeFitType(omxFitFunction *oo, const char *fitType);
void omxCompleteFitFunction(omxMatrix *om);
void setFreeVarGroup(omxFitFunction *ff, FreeVarGroup *fvg);

	void omxFreeFitFunctionArgs(omxFitFunction* fitFunction);						// Frees all args
	void omxGetFitFunctionStandardErrors(omxFitFunction *oo);					// Get Standard Errors

/* FitFunction-specific implementations of matrix functions */
void omxFitFunctionCompute(omxFitFunction *off, int want, FitContext *fc);
void omxFitFunctionComputeAuto(omxFitFunction *off, int want, FitContext *fc);
void omxFitFunctionComputeCI(omxFitFunction *off, int want, FitContext *fc);
	void omxDuplicateFitMatrix(omxMatrix *tgt, const omxMatrix *src, omxState* targetState);
void omxFitFunctionPreoptimize(omxFitFunction *off, FitContext *fc);
	
void omxFitFunctionPrint(omxFitFunction *source, const char* d);
	
omxMatrix* omxNewMatrixFromSlot(SEXP rObj, omxState* state, const char* slotName);

void omxInitFIMLFitFunction(omxFitFunction* off);
void omxInitAlgebraFitFunction(omxFitFunction *off);
void omxInitWLSFitFunction(omxFitFunction *off);
void omxInitRowFitFunction(omxFitFunction *off);
void omxInitMLFitFunction(omxFitFunction *off);
void omxInitRFitFunction(omxFitFunction *off);
void omxInitFitFunctionBA81(omxFitFunction* oo);
void ba81SetFreeVarGroup(omxFitFunction *oo, FreeVarGroup *fvg);
void omxInitGREMLFitFunction(omxFitFunction *oo);
void InitFellnerFitFunction(omxFitFunction *oo);
void ssMLFitInit(omxFitFunction* oo);

void ComputeFit(const char *callerName, omxMatrix *fitMat, int want, FitContext *fc);
void loglikelihoodCIFun(omxFitFunction* oo, int ffcompute, FitContext *fc);

double totalLogLikelihood(omxMatrix *fitMat);

const char *fitUnitsToName(int units);

#endif /* _OMXFITFUNCTION_H_ */
