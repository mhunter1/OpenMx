/*
  Copyright 2012-2014 Joshua Nathaniel Pritikin and contributors

  This is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <limits>
#include <Rmath.h>

#include "omxExpectationBA81.h"
#include "glue.h"
#include "libifa-rpf.h"
#include "dmvnorm.h"
#include "omxBuffer.h"
#include "matrix.h"

const struct rpf *rpf_model = NULL;
int rpf_numModels;

void pda(const double *ar, int rows, int cols)
{
	if (rows == 0 || cols == 0) return;
	std::string buf;
	for (int rx=0; rx < rows; rx++) {   // column major order
		for (int cx=0; cx < cols; cx++) {
			buf += string_snprintf("%.6g, ", ar[cx * rows + rx]);
		}
		buf += "\n";
	}
	mxLogBig(buf);
}

void pia(const int *ar, int rows, int cols)
{
	if (rows == 0 || cols == 0) return;
	std::string buf;
	for (int rx=0; rx < rows; rx++) {   // column major order
		for (int cx=0; cx < cols; cx++) {
			buf += string_snprintf("%d, ", ar[cx * rows + rx]);
		}
		buf += "\n";
	}
	mxLogBig(buf);
}

template <typename T>
void BA81LatentFixed<T>::normalizeWeights(class ifaGroup *grp, T extraData,
					  int px, double *Qweight, double patternLik1, int thrId)
{
	double weight = grp->rowWeight[px] / patternLik1;
	int pts = grp->quad.weightTableSize;
	for (int qx=0; qx < pts; ++qx) {
		Qweight[qx] *= weight;
	}
}

template <typename T>
void BA81LatentSummary<T>::begin(class ifaGroup *state, T extraData)
{
	thrDweight.assign(state->quad.weightTableSize * Global->numThreads, 0.0);
	ba81NormalQuad &quad = state->quad;
	numLatents = quad.maxAbilities + triangleLoc1(quad.maxAbilities);
	latentDist.assign(numLatents, 0.0);
}

template <typename T>
void BA81LatentSummary<T>::normalizeWeights(class ifaGroup *grp, T extraData,
					    int px, double *Qweight, double patternLik1, int thrId)
{
	double weight = grp->rowWeight[px] / patternLik1;
	int pts = grp->quad.weightTableSize;
	double *Dweight = thrDweight.data() + pts * thrId;
	for (int qx=0; qx < pts; ++qx) {
		double tmp = Qweight[qx] * weight;
		Dweight[qx] += tmp;
		Qweight[qx] = tmp;
	}
}

template <typename T>
void BA81LatentSummary<T>::end(class ifaGroup *grp, T extraData)
{
	int pts = grp->quad.weightTableSize;

	for (int tx=1; tx < Global->numThreads; ++tx) {
		double *Dweight = thrDweight.data() + pts * tx;
		double *dest = thrDweight.data();
		for (int qx=0; qx < pts; ++qx) {
			dest[qx] += Dweight[qx];
		}
	}

	ba81NormalQuad &quad = grp->quad;
	quad.EAP(thrDweight.data(), 1/extraData->weightSum, latentDist.data());

	omxMatrix *meanOut = extraData->estLatentMean;
	omxMatrix *covOut = extraData->estLatentCov;
	const int maxAbilities = quad.maxAbilities;
	const int primaryDims = quad.primaryDims;

	double *latentDist1 = latentDist.data();
	for (int d1=0; d1 < maxAbilities; d1++) {
		omxSetVectorElement(meanOut, d1, latentDist1[d1]);
	}

	for (int d1=0; d1 < primaryDims; d1++) {
		int cx = maxAbilities + triangleLoc1(d1);
		for (int d2=0; d2 <= d1; d2++) {
			double cov = latentDist1[cx];
			omxSetMatrixElement(covOut, d1, d2, cov);
			if (d1 != d2) omxSetMatrixElement(covOut, d2, d1, cov);
			++cx;
		}
	}
	for (int d1=primaryDims; d1 < maxAbilities; d1++) {
		int loc = maxAbilities + triangleLoc0(d1);
		omxSetMatrixElement(covOut, d1, d1, latentDist1[loc]);
	}

	++extraData->ElatentVersion;
}

template <typename T, typename CovType>
void BA81Estep<T, CovType>::begin(ifaGroup *state, T extraData)
{
	ba81NormalQuad &quad = state->quad;
	thrExpected.assign(state->totalOutcomes * quad.totalQuadPoints * Global->numThreads, 0.0);
}

template <typename T, typename CovType>
void BA81Estep<T, CovType>::addRow(class ifaGroup *state, T extraData, int px, double *Qweight, int thrId)
{
	double *out = thrExpected.data() + thrId * state->totalOutcomes * state->quad.totalQuadPoints;
	BA81EstepBase<CovType>::addRow1(state, px, Qweight, out);
}

template<>
void BA81EstepBase<BA81Dense>::addRow1(class ifaGroup *grp, int px, double *Qweight, double *out)
{
	std::vector<int> &rowMap = grp->rowMap;
	std::vector<int> &itemOutcomes = grp->itemOutcomes;
	ba81NormalQuad &quad = grp->getQuad();
	const int totalQuadPoints = quad.totalQuadPoints;

	for (int ix=0; ix < grp->numItems(); ++ix) {
		int pick = grp->dataColumns[ix][rowMap[px]];
		if (pick == NA_INTEGER) {
			out += itemOutcomes[ix] * totalQuadPoints;
			continue;
		}
		pick -= 1;

		for (int qx=0; qx < totalQuadPoints; ++qx) {
			out[pick] += Qweight[qx];
			out += itemOutcomes[ix];
		}
	}
}

template<>
void BA81EstepBase<BA81TwoTier>::addRow1(class ifaGroup *grp, int px, double *Qweight, double *out)
{
	std::vector<int> &rowMap = grp->rowMap;
	std::vector<int> &itemOutcomes = grp->itemOutcomes;
	ba81NormalQuad &quad = grp->getQuad();
	const int numSpecific = quad.numSpecific;
	const int totalQuadPoints = quad.totalQuadPoints;

	for (int ix=0; ix < grp->numItems(); ++ix) {
		int pick = grp->dataColumns[ix][rowMap[px]];
		if (pick == NA_INTEGER) {
			out += itemOutcomes[ix] * totalQuadPoints;
			continue;
		}
		pick -= 1;

		int Sgroup = grp->Sgroup[ix];
		double *Qw = Qweight;
		for (int qx=0; qx < totalQuadPoints; ++qx) {
			out[pick] += Qw[Sgroup];
			out += itemOutcomes[ix];
			Qw += numSpecific;
		}
	}
}

template <typename T, typename CovType>
void BA81Estep<T, CovType>::recordTable(class ifaGroup *state, T extraData)
{
	const int numThreads = Global->numThreads;
	ba81NormalQuad &quad = state->getQuad();
	const int expectedSize = quad.totalQuadPoints * state->totalOutcomes;
	double *e1 = thrExpected.data();

	extraData->expected = Realloc(extraData->expected, state->totalOutcomes * quad.totalQuadPoints, double);
	memcpy(extraData->expected, e1, sizeof(double) * expectedSize);
	e1 += expectedSize;

	for (int tx=1; tx < numThreads; ++tx) {
		for (int ex=0; ex < expectedSize; ++ex) {
			extraData->expected[ex] += *e1;
			++e1;
		}
	}
}

static int getLatentVersion(BA81Expect *state)
{
	int vv = 1;  // to ensure it doesn't match on the first test
	if (state->latentMeanOut) vv += omxGetMatrixVersion(state->latentMeanOut);
	if (state->latentCovOut) vv += omxGetMatrixVersion(state->latentCovOut);
	return vv;
}

// Attempt G-H grid? http://dbarajassolano.wordpress.com/2012/01/26/on-sparse-grid-quadratures/
void ba81SetupQuadrature(omxExpectation* oo)
{
	BA81Expect *state = (BA81Expect *) oo->argStruct;
	ba81NormalQuad &quad = state->getQuad();
	bool latentClean = state->latentParamVersion == getLatentVersion(state);
	if (quad.Qpoint.size() == 0 && latentClean) return;

	int maxAbilities = state->grp.maxAbilities;

	if (state->verbose >= 1) {
		mxLog("%s: quadrature(%d)", oo->name, getLatentVersion(state));
		if (state->verbose >= 2) {
			if (state->latentMeanOut) pda(state->latentMeanOut->data, 1, maxAbilities);
			if (state->latentCovOut)  pda(state->latentCovOut->data, maxAbilities, maxAbilities);
		}
	}

	if (maxAbilities == 0) {
		quad.setup0();
		state->latentParamVersion = getLatentVersion(state);
		return;
	}

	int numSpecific = state->grp.numSpecific;
	int priDims = maxAbilities - state->grp.numSpecific;
	Eigen::MatrixXd cov(priDims, priDims);
	Eigen::VectorXd sVar(numSpecific);

	omxMatrix *inputCov = state->latentCovOut;
	if (inputCov) {
		for (int d1=0; d1 < priDims; ++d1) {
			for (int d2=0; d2 < priDims; ++d2) {
				cov(d1, d2) = omxMatrixElement(inputCov, d1, d2);
			}
		}

		// This is required because the EM acceleration can push the
		// covariance matrix to be slightly non-pd when predictors
		// are highly correlated.

		if (priDims == 1) {
			if (cov(0,0) < BA81_MIN_VARIANCE) cov(0,0) = BA81_MIN_VARIANCE;
		} else {
			Matrix mat(cov.data(), priDims, priDims);
			InplaceForcePosSemiDef(mat, NULL, NULL);
		}

		//pda(inputCov->data, inputCov->rows, inputCov->cols);

		for (int sx=0; sx < numSpecific; ++sx) {
			int loc = priDims + sx;
			double tmp = inputCov->data[loc * inputCov->rows + loc];
			if (tmp < BA81_MIN_VARIANCE) tmp = BA81_MIN_VARIANCE;
			sVar(sx) = tmp;
		}
	} else {
		cov.setIdentity();
		sVar.setOnes();
	}

	if (state->latentMeanOut) {
		quad.setup(state->grp.qwidth, state->grp.qpoints, state->latentMeanOut->data, cov, sVar);
	} else {
		std::vector<double> meanVec(maxAbilities);
		quad.setup(state->grp.qwidth, state->grp.qpoints, meanVec.data(), cov, sVar);
	}

	state->latentParamVersion = getLatentVersion(state);
}

void refreshPatternLikelihood(BA81Expect *state, bool hasFreeLatent)
{
	ba81NormalQuad &quad = state->getQuad();

	if (hasFreeLatent) {
		if (quad.numSpecific == 0) {
			BA81Engine<typeof(state), BA81Dense, BA81LatentSummary, BA81OmitEstep> engine;
			engine.ba81Estep1(&state->grp, state);
		} else {
			BA81Engine<typeof(state), BA81TwoTier, BA81LatentSummary, BA81OmitEstep> engine;
			engine.ba81Estep1(&state->grp, state);
		}
	} else {
		if (quad.numSpecific == 0) {
			BA81Engine<typeof(state), BA81Dense, BA81LatentFixed, BA81OmitEstep> engine;
			engine.ba81Estep1(&state->grp, state);
		} else {
			BA81Engine<typeof(state), BA81TwoTier, BA81LatentFixed, BA81OmitEstep> engine;
			engine.ba81Estep1(&state->grp, state);
		}
	}
}

static void
ba81compute(omxExpectation *oo, const char *what, const char *how)
{
	BA81Expect *state = (BA81Expect *) oo->argStruct;

	if (what) {
		if (strcmp(what, "latentDistribution")==0 && how && strcmp(how, "copy")==0) {
			omxCopyMatrix(state->latentMeanOut, state->estLatentMean);
			omxCopyMatrix(state->latentCovOut, state->estLatentCov);
			return;
		}

		if (strcmp(what, "scores")==0) {
			state->type = EXPECTATION_AUGMENTED;
		} else if (strcmp(what, "nothing")==0) {
			state->type = EXPECTATION_OBSERVED;
		} else {
			omxRaiseErrorf("%s: don't know how to predict '%s'",
				       oo->name, what);
		}

		if (state->verbose >= 1) {
			mxLog("%s: predict %s", oo->name, what);
		}
		return;
	}

	bool latentClean = state->latentParamVersion == getLatentVersion(state);
	bool itemClean = state->itemParamVersion == omxGetMatrixVersion(state->itemParam) && latentClean;

	ba81NormalQuad &quad = state->getQuad();

	if (state->verbose >= 1) {
		mxLog("%s: Qinit %d itemClean %d latentClean %d (1=clean) expectedUsed=%d",
		      oo->name, quad.Qpoint.size() != 0, itemClean, latentClean, state->expectedUsed);
	}

	if (!latentClean) ba81SetupQuadrature(oo);

	if (!itemClean) {
		double *param = state->EitemParam? state->EitemParam : state->itemParam->data;
		state->grp.ba81OutcomeProb(param, FALSE);

		bool estep = state->expectedUsed;
		if (state->expectedUsed) {
			if (quad.numSpecific == 0) {
				if (oo->dynamicDataSource) {
					BA81Engine<typeof(state), BA81Dense, BA81LatentSummary, BA81Estep> engine;
					engine.ba81Estep1(&state->grp, state);
				} else {
					BA81Engine<typeof(state), BA81Dense, BA81LatentFixed, BA81Estep> engine;
					engine.ba81Estep1(&state->grp, state);
				}
			} else {
				if (oo->dynamicDataSource) {
					BA81Engine<typeof(state), BA81TwoTier, BA81LatentSummary, BA81Estep> engine;
					engine.ba81Estep1(&state->grp, state);
				} else {
					BA81Engine<typeof(state), BA81TwoTier, BA81LatentFixed, BA81Estep> engine;
					engine.ba81Estep1(&state->grp, state);
				}
			}
			state->expectedUsed = false;
		} else {
			Free(state->expected);
			refreshPatternLikelihood(state, oo->dynamicDataSource);
		}
		if (oo->dynamicDataSource && state->verbose >= 2) {
			omxPrint(state->estLatentMean, "mean");
			omxPrint(state->estLatentCov, "cov");
		}
		if (state->verbose >= 1) {
			const int numUnique = state->getNumUnique();
			mxLog("%s: estep(item version %d)<%s, %s, %s> %d/%d rows excluded",
			      state->name, omxGetMatrixVersion(state->itemParam),
			      (quad.numSpecific == 0? "dense":"twotier"),
			      (estep && oo->dynamicDataSource? "summary":"fixed"),
			      (estep? "estep":"omitEstep"),
			      state->grp.excludedPatterns, numUnique);
		}
	}

	state->itemParamVersion = omxGetMatrixVersion(state->itemParam);
}

/**
 * MAP is not affected by the number of items. EAP is. Likelihood can
 * get concentrated in a single quadrature ordinate. For 3PL, response
 * patterns can have a bimodal likelihood. This will confuse MAP and
 * is a key advantage of EAP (Thissen & Orlando, 2001, p. 136).
 *
 * Thissen, D. & Orlando, M. (2001). IRT for items scored in two
 * categories. In D. Thissen & H. Wainer (Eds.), \emph{Test scoring}
 * (pp 73-140). Lawrence Erlbaum Associates, Inc.
 */
static void
ba81PopulateAttributes(omxExpectation *oo, SEXP robj)
{
	BA81Expect *state = (BA81Expect *) oo->argStruct;
	ba81NormalQuad &quad = state->getQuad();
	int maxAbilities = quad.maxAbilities;
	const int numUnique = state->getNumUnique();

	Rf_setAttrib(robj, Rf_install("numStats"), Rf_ScalarReal(numUnique - 1)); // missingness? latent params? TODO

	if (state->debugInternal) {
		const double LogLargest = state->LogLargestDouble;
		int totalOutcomes = state->totalOutcomes();
		SEXP Rlik;
		SEXP Rexpected;

		if (state->grp.patternLik.size() != numUnique) {
			refreshPatternLikelihood(state, oo->dynamicDataSource);
		}

		Rf_protect(Rlik = Rf_allocVector(REALSXP, numUnique));
		memcpy(REAL(Rlik), state->grp.patternLik.data(), sizeof(double) * numUnique);
		double *lik_out = REAL(Rlik);
		for (int px=0; px < numUnique; ++px) {
			// Must return value in log units because it may not be representable otherwise
			lik_out[px] = log(lik_out[px]) - LogLargest;
		}

		MxRList dbg;
		dbg.add("patternLikelihood", Rlik);

		if (state->expected) {
			Rf_protect(Rexpected = Rf_allocVector(REALSXP, quad.totalQuadPoints * totalOutcomes));
			memcpy(REAL(Rexpected), state->expected, sizeof(double) * totalOutcomes * quad.totalQuadPoints);
			dbg.add("em.expected", Rexpected);
		}

		SEXP Rmean, Rcov;
		if (state->estLatentMean) {
			Rf_protect(Rmean = Rf_allocVector(REALSXP, maxAbilities));
			memcpy(REAL(Rmean), state->estLatentMean->data, maxAbilities * sizeof(double));
			dbg.add("mean", Rmean);
		}
		if (state->estLatentCov) {
			Rf_protect(Rcov = Rf_allocMatrix(REALSXP, maxAbilities, maxAbilities));
			memcpy(REAL(Rcov), state->estLatentCov->data, maxAbilities * maxAbilities * sizeof(double));
			dbg.add("cov", Rcov);
		}

		Rf_setAttrib(robj, Rf_install("debug"), dbg.asR());
	}
}

static void ba81Destroy(omxExpectation *oo) {
	if(OMX_DEBUG) {
		mxLog("Freeing %s function.", oo->name);
	}
	BA81Expect *state = (BA81Expect *) oo->argStruct;
	omxFreeMatrix(state->estLatentMean);
	omxFreeMatrix(state->estLatentCov);
	omxFreeMatrix(state->numObsMat);
	Free(state->expected);
	delete state;
}

static void ignoreSetVarGroup(omxExpectation*, FreeVarGroup *)
{}

static omxMatrix *getComponent(omxExpectation *oo, omxFitFunction*, const char *what)
{
	BA81Expect *state = (BA81Expect *) oo->argStruct;

	if (strcmp(what, "covariance")==0) {
		return state->estLatentCov;
	} else if (strcmp(what, "mean")==0) {
		return state->estLatentMean;
	} else if (strcmp(what, "numObs")==0) {
		return state->numObsMat;
	} else {
		return NULL;
	}
}

void getMatrixDims(SEXP r_theta, int *rows, int *cols)
{
    SEXP matrixDims;
    Rf_protect(matrixDims = Rf_getAttrib(r_theta, R_DimSymbol));
    int *dimList = INTEGER(matrixDims);
    *rows = dimList[0];
    *cols = dimList[1];
    Rf_unprotect(1);
}

void omxInitExpectationBA81(omxExpectation* oo) {
	omxState* currentState = oo->currentState;	
	SEXP rObj = oo->rObj;
	SEXP tmp;
	
	if(OMX_DEBUG) {
		mxLog("Initializing %s.", oo->name);
	}
	if (!rpf_model) {
		if (0) {
			const int wantVersion = 3;
			int version;
			get_librpf_t get_librpf = (get_librpf_t) R_GetCCallable("rpf", "get_librpf_model_GPL");
			(*get_librpf)(&version, &rpf_numModels, &rpf_model);
			if (version < wantVersion) Rf_error("librpf binary API %d installed, at least %d is required",
							 version, wantVersion);
		} else {
			rpf_numModels = librpf_numModels;
			rpf_model = librpf_model;
		}
	}
	
	BA81Expect *state = new BA81Expect;

	// These two constants should be as identical as possible
	state->name = oo->name;
	state->LogLargestDouble = log(std::numeric_limits<double>::max()) - 1;
	state->LargestDouble = exp(state->LogLargestDouble);
	ba81NormalQuad &quad = state->getQuad();
	quad.setOne(state->LargestDouble);

	state->expectedUsed = true;

	state->numObsMat = NULL;
	state->estLatentMean = NULL;
	state->estLatentCov = NULL;
	state->expected = NULL;
	state->type = EXPECTATION_OBSERVED;
	state->itemParam = NULL;
	state->EitemParam = NULL;
	state->itemParamVersion = 0;
	state->latentParamVersion = 0;
	oo->argStruct = (void*) state;

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("data")));
	state->data = omxDataLookupFromState(tmp, currentState);

	if (strcmp(omxDataType(state->data), "raw") != 0) {
		omxRaiseErrorf("%s unable to handle data type %s", oo->name, omxDataType(state->data));
		return;
	}

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("verbose")));
	state->verbose = Rf_asInteger(tmp);

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("qpoints")));
	int targetQpoints = Rf_asInteger(tmp);

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("qwidth")));
	state->grp.setGridFineness(Rf_asReal(tmp), targetQpoints);

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("ItemSpec")));
	state->grp.importSpec(tmp);
	if (state->verbose >= 2) mxLog("%s: found %d item specs", oo->name, state->numItems());

	state->latentMeanOut = omxNewMatrixFromSlot(rObj, currentState, "mean");
	state->latentCovOut  = omxNewMatrixFromSlot(rObj, currentState, "cov");

	state->itemParam = omxNewMatrixFromSlot(rObj, globalState, "item");
	state->grp.param = state->itemParam->data; // algebra not allowed yet TODO

	const int numItems = state->itemParam->cols;
	if (state->numItems() != numItems) {
		omxRaiseErrorf("ItemSpec length %d must match the number of item columns (%d)",
			       state->numItems(), numItems);
		return;
	}
	if (state->itemParam->rows != state->grp.paramRows) {
		omxRaiseErrorf("item matrix must have %d rows", state->grp.paramRows);
		return;
	}

	// for algebra item param, will need to defer until later?
	state->grp.learnMaxAbilities();

	int maxAbilities = state->grp.maxAbilities;

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("EstepItem")));
	if (!Rf_isNull(tmp)) {
		int rows, cols;
		getMatrixDims(tmp, &rows, &cols);
		if (rows != state->itemParam->rows || cols != state->itemParam->cols) {
			Rf_error("EstepItem must have the same dimensions as the item MxMatrix");
		}
		state->EitemParam = REAL(tmp);
	}

	oo->computeFun = ba81compute;
	oo->setVarGroup = ignoreSetVarGroup;
	oo->destructFun = ba81Destroy;
	oo->populateAttrFun = ba81PopulateAttributes;
	oo->componentFun = getComponent;
	oo->canDuplicate = false;
	
	// TODO: Exactly identical rows do not contribute any information.
	// The sorting algorithm ought to remove them so we get better cache behavior.
	// The following summary stats would be cheaper to calculate too.

	omxData *data = state->data;
	std::vector<int> &rowMap = state->grp.rowMap;

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("weightColumn")));
	int weightCol = INTEGER(tmp)[0];
	if (weightCol == NA_INTEGER) {
		// Should rowMap be part of omxData? This is essentially a
		// generic compression step that shouldn't be specific to IFA models.
		state->grp.rowWeight = (double*) R_alloc(data->rows, sizeof(double));
		rowMap.resize(data->rows);
		int numUnique = 0;
		for (int rx=0; rx < data->rows; ) {
			int rw = omxDataNumIdenticalRows(state->data, rx);
			state->grp.rowWeight[numUnique] = rw;
			rowMap[numUnique] = rx;
			rx += rw;
			++numUnique;
		}
		rowMap.resize(numUnique);
		state->weightSum = state->data->rows;
	}
	else {
		if (omxDataColumnIsFactor(data, weightCol)) {
			omxRaiseErrorf("%s: weightColumn %d is a factor", oo->name, 1 + weightCol);
			return;
		}
		state->grp.rowWeight = omxDoubleDataColumn(data, weightCol);
		state->weightSum = 0;
		for (int rx=0; rx < data->rows; ++rx) { state->weightSum += state->grp.rowWeight[rx]; }
		rowMap.resize(data->rows);
		for (size_t rx=0; rx < rowMap.size(); ++rx) {
			rowMap[rx] = rx;
		}
	}
	// complain about non-integral rowWeights (EAP can't work) TODO

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("dataColumns")));
	if (Rf_length(tmp) != numItems) Rf_error("dataColumns must be length %d", numItems);
	const int *colMap = INTEGER(tmp);

	for (int cx = 0; cx < numItems; cx++) {
		int *col = omxIntDataColumnUnsafe(data, colMap[cx]);
		state->grp.dataColumns.push_back(col);
	}

	// sanity check data
	for (int cx = 0; cx < numItems; cx++) {
		if (!omxDataColumnIsFactor(data, colMap[cx])) {
			omxRaiseErrorf("%s: column %d is not a factor", oo->name, 1 + colMap[cx]);
			return;
		}

		const int *col = state->grp.dataColumns[cx];

		// TODO this summary stat should be available from omxData
		int dataMax=0;
		for (int rx=0; rx < data->rows; rx++) {
			int pick = col[rx];
			if (dataMax < pick)
				dataMax = pick;
		}
		int no = state->grp.itemOutcomes[cx];
		if (dataMax > no) {
			omxRaiseErrorf("Data for item %d has %d outcomes, not %d", cx+1, dataMax, no);
		}
	}

	if (state->latentMeanOut && state->latentMeanOut->rows * state->latentMeanOut->cols != maxAbilities) {
		Rf_error("The mean matrix '%s' must be a row or column vector of size %d",
		      state->latentMeanOut->name, maxAbilities);
	}

	if (state->latentCovOut && (state->latentCovOut->rows != maxAbilities ||
				    state->latentCovOut->cols != maxAbilities)) {
		Rf_error("The cov matrix '%s' must be %dx%d",
		      state->latentCovOut->name, maxAbilities, maxAbilities);
	}

	state->grp.setLatentDistribution(maxAbilities,
					 state->latentMeanOut? state->latentMeanOut->data : NULL,
					 state->latentCovOut? state->latentCovOut->data : NULL);
	state->grp.detectTwoTier();

	if (state->verbose >= 1 && state->grp.numSpecific) {
		mxLog("%s: Two-tier structure detected; "
		      "%d abilities reduced to %d dimensions",
		      oo->name, maxAbilities, maxAbilities - state->grp.numSpecific + 1);
	}

	// TODO: Items with zero loadings can be replaced with equivalent items
	// with fewer factors. This would speed up calculation of derivatives.

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("minItemsPerScore")));
	state->grp.setMinItemsPerScore(Rf_asInteger(tmp));

	state->grp.sanityCheck();

	state->grp.buildRowSkip();

	if (isErrorRaised()) return;

	Rf_protect(tmp = R_do_slot(rObj, Rf_install("debugInternal")));
	state->debugInternal = Rf_asLogical(tmp);

	state->ElatentVersion = 0;
	if (state->latentMeanOut) {
		state->estLatentMean = omxInitMatrix(maxAbilities, 1, TRUE, currentState);
		omxCopyMatrix(state->estLatentMean, state->latentMeanOut); // rename matrices TODO
	}
	if (state->latentCovOut) {
		state->estLatentCov = omxInitMatrix(maxAbilities, maxAbilities, TRUE, currentState);
		omxCopyMatrix(state->estLatentCov, state->latentCovOut);
	}
	state->numObsMat = omxInitMatrix(1, 1, TRUE, currentState);
	omxSetVectorElement(state->numObsMat, 0, data->rows);
}
