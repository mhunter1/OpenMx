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

/***********************************************************
* 
*  omxExpectation.cc
*
*  Created: Timothy R. Brick 	Date: 2008-11-13 12:33:06
*
*	Expectation objects carry distributional expectations
* 		for the model.  Because they have no requirement
*		to produce a single matrix of output, they are
*		not a subclass of mxMatrix, but rather their own
*		strange beast.
*	// TODO:  Create a multi-matrix Algebra type, and make
*	//	MxExpectation a subtype of that.
*
**********************************************************/

#include "omxExpectation.h"

#ifdef SHADOW_DIAG
#pragma GCC diagnostic warning "-Wshadow"
#endif

typedef struct omxExpectationTableEntry omxExpectationTableEntry;

struct omxExpectationTableEntry {
	char name[32];
	void (*initFun)(omxExpectation*);
};

static const omxExpectationTableEntry omxExpectationSymbolTable[] = {
	{"MxExpectationLISREL",			&omxInitLISRELExpectation},
	{"MxExpectationStateSpace",			&omxInitStateSpaceExpectation},
	{"MxExpectationNormal", 		&omxInitNormalExpectation},
	{"MxExpectationRAM",			&omxInitRAMExpectation},
	{"MxExpectationBA81", &omxInitExpectationBA81},
  {"MxExpectationGREML", &omxInitGREMLExpectation}
};

void omxFreeExpectationArgs(omxExpectation *ox) {
	if(ox==NULL) return;
    
	if (ox->destructFun) ox->destructFun(ox);
	Free(ox);
}

void omxExpectationRecompute(FitContext *fc, omxExpectation *ox)
{
	omxExpectationCompute(fc, ox, NULL);
}

void omxExpectationCompute(FitContext *fc, omxExpectation *ox, const char *what, const char *how)
{
	if (!ox) return;

	ox->data->recompute(); // for dynamic data
	ox->computeFun(ox, fc, what, how);
}

omxMatrix* omxGetExpectationComponent(omxExpectation* ox, const char* component)
{
	if(component == NULL) return NULL;

	if(ox->componentFun == NULL) return NULL;

	return(ox->componentFun(ox, component));
}

void omxSetExpectationComponent(omxExpectation* ox, const char* component, omxMatrix* om)
{
	ox->mutateFun(ox, component, om);
}

omxExpectation* omxDuplicateExpectation(const omxExpectation *src, omxState* newState) {

	return omxNewIncompleteExpectation(src->rObj, src->expNum, newState);
}

omxExpectation* omxExpectationFromIndex(int expIndex, omxState* os)
{
	omxExpectation* ox = os->expectationList.at(expIndex);
	return ox;
}

static void omxExpectationProcessDataStructures(omxExpectation* ox, SEXP rObj)
{
	int index, numCols, numOrdinal=0;
	SEXP nextMatrix, itemList, threshMatrix; 
	
	if(rObj == NULL) return;

	{
		ScopedProtect p1(nextMatrix, R_do_slot(rObj, Rf_install("dataColumns")));
		ox->saveDataColumnsInfo(nextMatrix);
	}

	if(OMX_DEBUG) {
		mxPrintMat("Variable mapping", ox->getDataColumns());
	}
	
	auto dc = ox->getDataColumns();
	numCols = dc.size();
	omxData *data = ox->data;
	for (int cx=0; cx < numCols; ++cx) {
		int var = dc[cx];
		data->assertColumnIsData(var);
	}

	if (R_has_slot(rObj, Rf_install("thresholds"))) {
		if(OMX_DEBUG) {
			mxLog("Accessing Threshold matrix.");
		}
		ScopedProtect p1(threshMatrix, R_do_slot(rObj, Rf_install("thresholds")));

		if(INTEGER(threshMatrix)[0] != NA_INTEGER) {
			if(OMX_DEBUG) {
				mxLog("Accessing Threshold Mappings.");
			}
        
			ox->thresholdsMat = omxMatrixLookupFromState1(threshMatrix, ox->currentState);

			/* Process the data and threshold mapping structures */
			/* if (threshMatrix == NA_INTEGER), then we could ignore the slot "thresholdColumns"
			 * and fill all the thresholds with {NULL, 0, 0}.
			 * However the current path does not have a lot of overhead. */
			int* thresholdColumn, *thresholdNumber;
			{ScopedProtect pc(nextMatrix, R_do_slot(rObj, Rf_install("thresholdColumns")));
				thresholdColumn = INTEGER(nextMatrix);
			}
			{ScopedProtect pi(itemList, R_do_slot(rObj, Rf_install("thresholdLevels")));
				thresholdNumber = INTEGER(itemList);
			}
			ox->thresholds.reserve(numCols);
			for(index = 0; index < numCols; index++) {
				if(thresholdColumn[index] == NA_INTEGER) {	// Continuous variable
					if(OMX_DEBUG) {
						mxLog("Column %d is continuous.", index);
					}
					omxThresholdColumn col;
					ox->thresholds.push_back(col);
				} else {
					omxThresholdColumn col;
					col.dColumn = index;
					col.column = thresholdColumn[index];
					col.numThresholds = thresholdNumber[index];
					ox->thresholds.push_back(col);
					if(OMX_DEBUG) {
						mxLog("Column %d is ordinal with %d thresholds in threshold column %d.", 
						      index, thresholdNumber[index], thresholdColumn[index]);
					}
					numOrdinal++;
				}
			}
			if(OMX_DEBUG) {
				mxLog("%d threshold columns processed.", numOrdinal);
			}
			ox->numOrdinal = numOrdinal;
		} else {
			if (OMX_DEBUG) {
				mxLog("No thresholds matrix; not processing thresholds.");
			}
			ox->numOrdinal = 0;
		}
	}
}

omxExpectation* omxNewIncompleteExpectation(SEXP rObj, int expNum, omxState* os) {

	SEXP ExpectationClass;
	const char *expType;
	{ScopedProtect p1(ExpectationClass, STRING_ELT(Rf_getAttrib(rObj, R_ClassSymbol), 0));
		expType = CHAR(ExpectationClass);
	}

	omxExpectation* expect = omxNewInternalExpectation(expType, os);

	expect->rObj = rObj;
	expect->expNum = expNum;
	
	SEXP nextMatrix;
	{ScopedProtect p1(nextMatrix, R_do_slot(rObj, Rf_install("data")));
	expect->data = omxDataLookupFromState(nextMatrix, os);
	}

	return expect;
}

void omxCompleteExpectation(omxExpectation *ox) {
	
	if(ox->isComplete) return;
	ox->isComplete = TRUE;

	omxExpectationProcessDataStructures(ox, ox->rObj);

	ox->initFun(ox);

	if(ox->computeFun == NULL) {
		if (isErrorRaised()) {
			Rf_error("Failed to initialize '%s' of type %s: %s", ox->name, ox->expType, Global->getBads());
		} else {
			Rf_error("Failed to initialize '%s' of type %s", ox->name, ox->expType);
		}
	}

	if (OMX_DEBUG) {
		omxData *od = ox->data;
		omxState *state = ox->currentState;
		std::string msg = string_snprintf("Expectation '%s' of type '%s' has"
						  " %d definition variables:\n", ox->name, ox->expType,
						  int(od->defVars.size()));
		for (int dx=0; dx < int(od->defVars.size()); ++dx) {
			omxDefinitionVar &dv = od->defVars[dx];
			msg += string_snprintf("[%d] column '%s' ->", dx, omxDataColumnName(od, dv.column));
			msg += string_snprintf(" %s[%d,%d]", state->matrixToName(~dv.matrix),
					       dv.row, dv.col);
			msg += "\n  dirty:";
			for (int mx=0; mx < dv.numDeps; ++mx) {
				msg += string_snprintf(" %s", state->matrixToName(dv.deps[mx]));
			}
			msg += "\n";
		}
		mxLogBig(msg);
	}
}

static void defaultSetVarGroup(omxExpectation *ox, FreeVarGroup *fvg)
{
	if (OMX_DEBUG && ox->freeVarGroup && ox->freeVarGroup != fvg) {
		Rf_warning("setFreeVarGroup called with different group (%d vs %d) on %s",
			ox->name, ox->freeVarGroup->id[0], fvg->id[0]);
	}
	ox->freeVarGroup = fvg;
}

void setFreeVarGroup(omxExpectation *ox, FreeVarGroup *fvg)
{
	(*ox->setVarGroup)(ox, fvg);
}

int *defaultDataColumnFun(omxExpectation *ex)
{ return ex->dataColumnsPtr; }

omxExpectation *
omxNewInternalExpectation(const char *expType, omxState* os)
{
	omxExpectation* expect = Calloc(1, omxExpectation);
	expect->setVarGroup = defaultSetVarGroup;

	/* Switch based on Expectation type. */ 
	for (size_t ex=0; ex < OMX_STATIC_ARRAY_SIZE(omxExpectationSymbolTable); ex++) {
		const omxExpectationTableEntry *entry = omxExpectationSymbolTable + ex;
		if(strEQ(expType, entry->name)) {
		        expect->expType = entry->name;
			expect->initFun = entry->initFun;
			break;
		}
	}

	if(!expect->initFun) {
		Free(expect);
		Rf_error("Expectation %s not implemented", expType);
	}

	expect->currentState = os;
	expect->canDuplicate = true;
	expect->dynamicDataSource = false;
	expect->dataColumnFun = defaultDataColumnFun;

	return expect;
}

void omxExpectationPrint(omxExpectation* ox, char* d) {
	if(ox->printFun != NULL) {
		ox->printFun(ox);
	} else {
		mxLog("(Expectation, type %s) ", (ox->expType==NULL?"Untyped":ox->expType));
	}
}

void complainAboutMissingMeans(omxExpectation *off)
{
	omxRaiseErrorf("%s: raw data observed but no expected means "
		       "vector was provided. Add something like mxPath(from = 'one',"
		       " to = manifests) to your model.", off->name);
}

bool omxExpectation::loadDefVars(int row)
{
	bool changed = false;
	for (int k=0; k < int(data->defVars.size()); ++k) {
		omxDefinitionVar &dv = data->defVars[k];
		double newDefVar = omxDoubleDataElement(data, row, dv.column);
		if(ISNA(newDefVar)) {
			Rf_error("Error: NA value for a definition variable is Not Yet Implemented.");
		}
		changed |= dv.loadData(currentState, newDefVar);
	}
	if (changed && OMX_DEBUG_ROWS(row)) { mxLog("%s: loading definition vars for row %d", name, row); }
	return changed;
}
