/*
 *  Copyright 2013, 2016 The OpenMx Project
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

#include "omxDefines.h"
#include "omxState.h"
#include "omxFitFunction.h"
#include "omxNPSOLSpecific.h"
#include "omxExportBackendState.h"
#include "omxCsolnp.h"
#include "nloptcpp.h"
#include "Compute.h"
#include "npsolswitch.h"
#include "glue.h"
#include "ComputeSD.h"

#ifdef SHADOW_DIAG
#pragma GCC diagnostic warning "-Wshadow"
#endif

enum OptEngine {
	OptEngine_NPSOL,
	OptEngine_CSOLNP,
    OptEngine_NLOPT,
    OptEngine_SD
};

class omxComputeGD : public omxCompute {
	typedef omxCompute super;
	const char *engineName;
	enum OptEngine engine;
	const char *gradientAlgoName;
	enum GradientAlgorithm gradientAlgo;
	int gradientIterations;
	double gradientStepSize;
	omxMatrix *fitMatrix;
	int verbose;
	double optimalityTolerance;
	int maxIter;
	std::vector<int> excludeVars;

	bool useGradient;
	SEXP hessChol;
	bool nudge;

	int warmStartSize;
	double *warmStart;
	int threads;

public:
	omxComputeGD();
	virtual void initFromFrontend(omxState *, SEXP rObj);
	virtual void computeImpl(FitContext *fc);
	virtual void reportResults(FitContext *fc, MxRList *slots, MxRList *out);
};

class omxCompute *newComputeGradientDescent()
{
	return new omxComputeGD();
}

omxComputeGD::omxComputeGD()
{
	hessChol = NULL;
	warmStart = NULL;
}

void omxComputeGD::initFromFrontend(omxState *globalState, SEXP rObj)
{
	super::initFromFrontend(globalState, rObj);

	SEXP slotValue;
	fitMatrix = omxNewMatrixFromSlot(rObj, globalState, "fitfunction");
	setFreeVarGroup(fitMatrix->fitFunction, varGroup);
	omxCompleteFitFunction(fitMatrix);

	ScopedProtect p1(slotValue, R_do_slot(rObj, Rf_install("verbose")));
	verbose = Rf_asInteger(slotValue);

	ScopedProtect p2(slotValue, R_do_slot(rObj, Rf_install("tolerance")));
	optimalityTolerance = Rf_asReal(slotValue);
	if (!std::isfinite(optimalityTolerance)) {
		optimalityTolerance = Global->optimalityTolerance;
	}

	ScopedProtect p3(slotValue, R_do_slot(rObj, Rf_install("engine")));
	engineName = CHAR(Rf_asChar(slotValue));
	if (strEQ(engineName, "CSOLNP")) {
		engine = OptEngine_CSOLNP;
	} else if (strEQ(engineName, "SLSQP")) {
		engine = OptEngine_NLOPT;
	} else if (strEQ(engineName, "NPSOL")) {
#if HAS_NPSOL
		engine = OptEngine_NPSOL;
#else
		Rf_error("NPSOL is not available in this build. See ?omxGetNPSOL() to download this optimizer");
#endif
	} else if(strEQ(engineName, "SD")){
		engine = OptEngine_SD;
	} else {
		Rf_error("%s: engine %s unknown", name, engineName);
	}

	ScopedProtect p5(slotValue, R_do_slot(rObj, Rf_install("useGradient")));
	if (Rf_length(slotValue)) {
		useGradient = Rf_asLogical(slotValue);
	} else {
		useGradient = Global->analyticGradients;
	}

	ScopedProtect p4(slotValue, R_do_slot(rObj, Rf_install("nudgeZeroStarts")));
	nudge = Rf_asLogical(slotValue);

	ScopedProtect p6(slotValue, R_do_slot(rObj, Rf_install("warmStart")));
	if (!Rf_isNull(slotValue)) {
		SEXP matrixDims;
		ScopedProtect pws(matrixDims, Rf_getAttrib(slotValue, R_DimSymbol));
		int *dimList = INTEGER(matrixDims);
		int rows = dimList[0];
		int cols = dimList[1];
		if (rows != cols) Rf_error("%s: warmStart matrix must be square", name);

		warmStartSize = rows;
		warmStart = REAL(slotValue);
	}

	ScopedProtect p7(slotValue, R_do_slot(rObj, Rf_install("maxMajorIter")));
	if (Rf_length(slotValue)) {
		maxIter = Rf_asInteger(slotValue);
	} else {
		maxIter = -1; // different engines have different defaults
	}

	ScopedProtect p8(slotValue, R_do_slot(rObj, Rf_install("gradientAlgo")));
	gradientAlgoName = CHAR(Rf_asChar(slotValue));
	if (strEQ(gradientAlgoName, "forward")) {
		gradientAlgo = GradientAlgorithm_Forward;
	} else if (strEQ(gradientAlgoName, "central")) {
		gradientAlgo = GradientAlgorithm_Central;
	} else {
		Rf_error("%s: gradient algorithm '%s' unknown", name, gradientAlgoName);
	}

	ScopedProtect p9(slotValue, R_do_slot(rObj, Rf_install("gradientIterations")));
	gradientIterations = std::max(Rf_asInteger(slotValue), 1);

	ScopedProtect p10(slotValue, R_do_slot(rObj, Rf_install("gradientStepSize")));
	gradientStepSize = Rf_asReal(slotValue);

	ProtectedSEXP Rexclude(R_do_slot(rObj, Rf_install(".excludeVars")));
	excludeVars.reserve(Rf_length(Rexclude));
	for (int ex=0; ex < Rf_length(Rexclude); ++ex) {
		int got = varGroup->lookupVar(CHAR(STRING_ELT(Rexclude, ex)));
		if (got < 0) continue;
		excludeVars.push_back(got);
	}
}

void omxComputeGD::computeImpl(FitContext *fc)
{
	omxAlgebraPreeval(fitMatrix, fc);
	if (isErrorRaised()) return;

	size_t numParam = fc->varGroup->vars.size();
	if (excludeVars.size()) {
		fc->profiledOut.assign(fc->numParam, false);
		for (auto vx : excludeVars) fc->profiledOut[vx] = true;
	}
	if (fc->profiledOut.size()) {
		if (fc->profiledOut.size() != fc->numParam) Rf_error("Fail");
		for (size_t vx=0; vx < fc->varGroup->vars.size(); ++vx) {
			if (fc->profiledOut[vx]) --numParam;
		}
	}

	if (numParam <= 0) {
		omxRaiseErrorf("%s: model has no free parameters", name);
		return;
	}

	fc->ensureParamWithinBox(nudge);
	fc->createChildren(fitMatrix);

	int beforeEval = fc->getLocalComputeCount();

	if (verbose >= 1) mxLog("%s: engine %s (ID %d) gradient=%s tol=%g constraints=%d",
				name, engineName, engine, gradientAlgoName, optimalityTolerance,
				int(fitMatrix->currentState->conListX.size()));

	//if (fc->ciobj) verbose=2;
	GradientOptimizerContext rf(fc, verbose);
	threads = rf.numOptimizerThreads;
	rf.fitMatrix = fitMatrix;
	rf.ControlTolerance = optimalityTolerance;
	rf.useGradient = useGradient;
	rf.gradientAlgo = gradientAlgo;
	rf.gradientIterations = gradientIterations;
	rf.gradientStepSize = gradientStepSize;
	if (maxIter == -1) {
		rf.maxMajorIterations = -1;
	} else {
		rf.maxMajorIterations = fc->iterations + maxIter;
	}
	if (warmStart) {
		if (warmStartSize != int(numParam)) {
			Rf_warning("warmStart size %d does not match number of free parameters %d (ignored)",
				   warmStartSize, numParam);
		} else {
			Eigen::Map< Eigen::MatrixXd > hessWrap(warmStart, numParam, numParam);
			rf.hessOut = hessWrap;
			rf.warmStart = true;
		}
	}

	switch (engine) {
        case OptEngine_NPSOL:{
#if HAS_NPSOL
		omxNPSOL(rf);
		rf.finish();
		fc->wanted |= FF_COMPUTE_GRADIENT;
		if (rf.hessOut.size() && fitMatrix->currentState->conListX.size() == 0) {
			if (!hessChol) {
				Rf_protect(hessChol = Rf_allocMatrix(REALSXP, numParam, numParam));
			}
			Eigen::Map<Eigen::MatrixXd> hc(REAL(hessChol), numParam, numParam);
			hc = rf.hessOut;
			Eigen::Map<Eigen::MatrixXd> dest(fc->getDenseHessUninitialized(), numParam, numParam);
			dest.noalias() = rf.hessOut.transpose() * rf.hessOut;
			fc->wanted |= FF_COMPUTE_HESSIAN;
		}
#endif
		break;}
        case OptEngine_CSOLNP:
		if (rf.maxMajorIterations == -1) rf.maxMajorIterations = Global->majorIterations;
		rf.avoidRedundentEvals = true;
		rf.CSOLNP_HACK = true;
		omxCSOLNP(rf);
		rf.finish();
		if (rf.gradOut.size()) {
			fc->grad = rf.gradOut.tail(numParam);
			Eigen::Map< Eigen::MatrixXd > hess(fc->getDenseHessUninitialized(), numParam, numParam);
			hess = rf.hessOut.bottomRightCorner(numParam, numParam);
			fc->wanted |= FF_COMPUTE_GRADIENT | FF_COMPUTE_HESSIAN;
		}
		break;
        case OptEngine_NLOPT:
		rf.gradientStepSize = rf.gradientStepSize * GRADIENT_FUDGE_FACTOR(2.0);
		if (rf.maxMajorIterations == -1) rf.maxMajorIterations = Global->majorIterations;
		omxInvokeNLOPT(rf);
		rf.finish();
		fc->wanted |= FF_COMPUTE_GRADIENT;
		break;
        case OptEngine_SD:{
		fc->copyParamToModel();
		rf.setupSimpleBounds();
		rf.setupIneqConstraintBounds();
		rf.solEqBFun();
		rf.myineqFun();
		if(rf.inequality.size() == 0 && rf.equality.size() == 0) {
			omxSD(rf);   // unconstrained problems
			rf.finish();
		} else {
			Rf_error("Constrained problems are not implemented");
		}
		fc->wanted |= FF_COMPUTE_GRADIENT;
		break;}
        default: Rf_error("Optimizer %d is not available", engine);
	}

	if (!std::isfinite(fc->fit)) {
		fc->setInform(INFORM_STARTING_VALUES_INFEASIBLE);
	} else {
		fc->setInform(rf.informOut);
		if (fc->ciobj) fc->ciobj->checkSolution(fc);
	}

	if (verbose >= 1) {
		mxLog("%s: engine %s done, iter=%d inform=%d",
		      name, engineName, fc->getLocalComputeCount() - beforeEval, fc->getInform());
	}

	if (isErrorRaised()) return;

	// Optimizers can terminate with inconsistent fit and parameters
	ComputeFit(name, fitMatrix, FF_COMPUTE_FIT, fc);

	if (verbose >= 2) {
		mxLog("%s: final fit is %2f", name, fc->fit);
		fc->log(FF_COMPUTE_ESTIMATE);
	}

	fc->wanted |= FF_COMPUTE_BESTFIT;
}

void omxComputeGD::reportResults(FitContext *fc, MxRList *slots, MxRList *out)
{
	omxPopulateFitFunction(fitMatrix, out);

	MxRList output;
	output.add("maxThreads", Rf_ScalarInteger(threads));
	slots->add("output", output.asR());

	if (engine == OptEngine_NPSOL && hessChol) {
		out->add("hessianCholesky", hessChol);
	}
}

// -----------------------------------------------------------------------

class ComputeCI : public omxCompute {
	typedef omxCompute super;
	typedef CIobjective::Diagnostic Diagnostic;
	omxCompute *plan;
	omxMatrix *fitMatrix;
	int verbose;
	SEXP intervals, intervalCodes, detail;
	const char *ctypeName;
	bool useInequality;

	enum Method {
		NEALE_MILLER_1997=1,
		WU_NEALE_2012
	};

	void regularCI(FitContext *mle, FitContext &fc, ConfidenceInterval *currentCI, int lower,
		       double &val, Diagnostic &better);
	void regularCI2(FitContext *mle, FitContext &fc, ConfidenceInterval *currentCI, int &detailRow);
	void boundAdjCI(FitContext *mle, FitContext &fc, ConfidenceInterval *currentCI, int &detailRow);
	void recordCI(Method meth, ConfidenceInterval *currentCI, int lower, FitContext &fc,
		      int &detailRow, double val, Diagnostic diag);
	void checkOtherBoxConstraints(FitContext &fc, ConfidenceInterval *currentCI, Diagnostic &diag);
public:
	ComputeCI();
	virtual void initFromFrontend(omxState *, SEXP rObj);
	virtual void computeImpl(FitContext *fc);
	virtual void reportResults(FitContext *fc, MxRList *slots, MxRList *out);
	virtual void collectResults(FitContext *fc, LocalComputeResult *lcr, MxRList *out);
};

omxCompute *newComputeConfidenceInterval()
{
	return new ComputeCI();
}

ComputeCI::ComputeCI()
{
	intervals = 0;
	intervalCodes = 0;
	detail = 0;
	useInequality = false;
}

void ComputeCI::initFromFrontend(omxState *globalState, SEXP rObj)
{
	super::initFromFrontend(globalState, rObj);

	SEXP slotValue;
	{
		ScopedProtect p1(slotValue, R_do_slot(rObj, Rf_install("verbose")));
		verbose = Rf_asInteger(slotValue);
	}
	{
		ScopedProtect p1(slotValue, R_do_slot(rObj, Rf_install("constraintType")));
		ctypeName = CHAR(Rf_asChar(slotValue));
		// really only constrained and unconstrained TODO
		if (strEQ(ctypeName, "ineq")) {
			useInequality = true;
		} else if (strEQ(ctypeName, "eq")) {
			useInequality = true;
		} else if (strEQ(ctypeName, "both")) {
			useInequality = true;
		} else if (strEQ(ctypeName, "none")) {
			// OK
		} else {
			Rf_error("%s: unknown constraintType='%s'", name, ctypeName);
		}
	}

	fitMatrix = omxNewMatrixFromSlot(rObj, globalState, "fitfunction");
	setFreeVarGroup(fitMatrix->fitFunction, varGroup);
	omxCompleteFitFunction(fitMatrix);

	Rf_protect(slotValue = R_do_slot(rObj, Rf_install("plan")));
	SEXP s4class;
	Rf_protect(s4class = STRING_ELT(Rf_getAttrib(slotValue, R_ClassSymbol), 0));
	plan = omxNewCompute(globalState, CHAR(s4class));
	plan->initFromFrontend(globalState, slotValue);
}

extern "C" { void F77_SUB(npoptn)(char* string, int Rf_length); };

class notImplementedConstraint : public omxConstraint {
	typedef omxConstraint super;
public:
	notImplementedConstraint() : super("not implemented") {};
	virtual void refreshAndGrab(FitContext *fc, Type ineqType, double *out) {
		Rf_error("Not implemented");
	};
	virtual omxConstraint *duplicate(omxState *dest) {
		Rf_error("Not implemented");
	}
};

class ciConstraint : public omxConstraint {
 private:
	typedef omxConstraint super;
	omxState *state;
public:
	omxMatrix *fitMat;
	ciConstraint() : super("CI"), state(0) {};
	virtual ~ciConstraint() {
		pop();
	};
	void push(omxState *_state) {
		state = _state;
		state->conListX.push_back(this);
	};
	void pop() {
		if (!state) return;
		size_t sz = state->conListX.size();
		if (sz && state->conListX[sz-1] == this) {
			state->conListX.pop_back();
		}
		state = 0;
	};
	virtual omxConstraint *duplicate(omxState *dest) {
		return new notImplementedConstraint();
	};
};

class ciConstraintIneq : public ciConstraint {
 private:
	typedef ciConstraint super;
 public:
	ciConstraintIneq(int _size)
	{ size=_size; opCode = LESS_THAN; };

	virtual void refreshAndGrab(FitContext *fc, Type ineqType, double *out) {
		fc->ciobj->evalIneq(fc, fitMat, out);
		Eigen::Map< Eigen::ArrayXd > Eout(out, size);
		if (ineqType != opCode) Eout = -Eout;
		//mxLog("fit %f diff %f", fit, diff);
	};
};

class ciConstraintEq : public ciConstraint {
 private:
	typedef ciConstraint super;
 public:
	ciConstraintEq(int _size)
	{ size=_size; opCode = EQUALITY; };

	virtual void refreshAndGrab(FitContext *fc, Type ineqType, double *out) {
		fc->ciobj->evalEq(fc, fitMat, out);
		//mxLog("fit %f diff %f", fit, diff);
	};
};

void ComputeCI::recordCI(Method meth, ConfidenceInterval *currentCI, int lower, FitContext &fc,
			 int &detailRow, double val, Diagnostic diag)
{
	omxMatrix *ciMatrix = currentCI->getMatrix(fitMatrix->currentState);
	std::string &matName = ciMatrix->nameStr;

	if (diag == CIobjective::DIAG_SUCCESS) {
		currentCI->val[!lower] = val;
		currentCI->code[!lower] = fc.getInform();
	}

	if(verbose >= 1) {
		mxLog("CI[%s,%s] %s[%d,%d] val=%f fit=%f status=%d accepted=%d",
		      currentCI->name.c_str(), (lower?"lower":"upper"), matName.c_str(),
		      1+currentCI->row, 1+currentCI->col, val, fc.fit, fc.getInform(), diag);
	}

	SET_STRING_ELT(VECTOR_ELT(detail, 0), detailRow, Rf_mkChar(currentCI->name.c_str()));
	REAL(VECTOR_ELT(detail, 1))[detailRow] = val;
	INTEGER(VECTOR_ELT(detail, 2))[detailRow] = 1+lower;
	REAL(VECTOR_ELT(detail, 3))[detailRow] = fc.fit;
	Eigen::Map< Eigen::VectorXd > Est(fc.est, fc.numParam);
	for (int px=0; px < int(fc.numParam); ++px) {
		REAL(VECTOR_ELT(detail, 4+px))[detailRow] = Est[px];
	}
	INTEGER(VECTOR_ELT(detail, 4+fc.numParam))[detailRow] = meth;
	INTEGER(VECTOR_ELT(detail, 5+fc.numParam))[detailRow] = diag;
	INTEGER(VECTOR_ELT(detail, 6+fc.numParam))[detailRow] = 1 + fc.getInform();
	++detailRow;
}

struct regularCIobj : CIobjective {
	double targetFit;
	bool lowerBound;
	bool compositeCIFunction;
	double diff;

	void computeConstraint(double fit)
	{
		diff = fit - targetFit;
	}

	virtual bool gradientKnown()
	{
		return CI->varIndex >= 0 && !compositeCIFunction;
	}

	virtual void gradient(FitContext *fc, double *gradOut) {
		Eigen::Map< Eigen::VectorXd > Egrad(gradOut, fc->numParam);
		Egrad.setZero();
		Egrad[ CI->varIndex ] = lowerBound? 1 : -1;
	}

	virtual void evalIneq(FitContext *fc, omxMatrix *fitMat, double *out)
	{
		omxFitFunctionCompute(fitMat->fitFunction, FF_COMPUTE_FIT, fc);
		const double fit = totalLogLikelihood(fitMat);
		computeConstraint(fit);
		*out = std::max(diff, 0.0);
	}

	virtual void evalFit(omxFitFunction *ff, int want, FitContext *fc)
	{
		omxMatrix *fitMat = ff->matrix;

		if (!(want & FF_COMPUTE_FIT)) {
			if (want & (FF_COMPUTE_PREOPTIMIZE | FF_COMPUTE_INITIAL_FIT)) return;
			Rf_error("Not implemented yet");
		}

		// We need to compute the fit here because that's the only way to
		// check our soft feasibility constraints. If parameters don't
		// change between here and the constraint evaluation then we
		// should avoid recomputing the fit again in the constraint. TODO

		omxFitFunctionCompute(fitMat->fitFunction, FF_COMPUTE_FIT, fc);
		const double fit = totalLogLikelihood(fitMat);
		omxMatrix *ciMatrix = CI->getMatrix(fitMat->currentState);
		omxRecompute(ciMatrix, fc);
		double CIElement = omxMatrixElement(ciMatrix, CI->row, CI->col);
		omxResizeMatrix(fitMat, 1, 1);

		if (!std::isfinite(fit)) {
			fc->recordIterationError("Confidence interval is in a range that is currently incalculable. Add constraints to keep the value in the region where it can be calculated.");
			fitMat->data[0] = nan("infeasible");
			return;
		}

		if (want & FF_COMPUTE_FIT) {
			double param = (lowerBound? CIElement : -CIElement);
			computeConstraint(fit);
			if (fabs(diff) > 100) param = nan("infeasible");
			if (compositeCIFunction) {
				fitMat->data[0] = diff*diff + param;
			} else {
				fitMat->data[0] = param;
			}
			//mxLog("param at %f", fitMat->data[0]);
		}
		if (want & (FF_COMPUTE_GRADIENT | FF_COMPUTE_HESSIAN | FF_COMPUTE_IHESSIAN)) {
			// add deriv adjustments here TODO
		}
	}

	virtual Diagnostic getDiag()
	{
		Diagnostic diag = DIAG_SUCCESS;
		if (fabs(diff) > 1e-1) {
			diag = DIAG_ALPHA_LEVEL;
		}
		return diag;
	}
};

struct bound1CIobj : CIobjective {
	double bound;
	bool constrained;
	Eigen::Array<double,1,1> eq;

	template <typename T1>
	void computeConstraint(FitContext *fc, omxMatrix *fitMat, double fit, Eigen::ArrayBase<T1> &v1)
	{
		omxMatrix *ciMatrix = CI->getMatrix(fitMat->currentState);
		omxRecompute(ciMatrix, fc);
		double CIElement = omxMatrixElement(ciMatrix, CI->row, CI->col);
		v1 << CIElement - bound;
		eq = v1;
		//mxPrintMat("v1", v1);
	}

	virtual void evalEq(FitContext *fc, omxMatrix *fitMat, double *out)
	{
		omxFitFunctionCompute(fitMat->fitFunction, FF_COMPUTE_FIT, fc);
		const double fit = totalLogLikelihood(fitMat);
		Eigen::Array<double,1,1> v1;
		computeConstraint(fc, fitMat, fit, v1);
		*out = v1(0);
	}

	virtual void evalFit(omxFitFunction *ff, int want, FitContext *fc)
	{
		omxMatrix *fitMat = ff->matrix;

		if (!(want & FF_COMPUTE_FIT)) {
			if (want & (FF_COMPUTE_PREOPTIMIZE | FF_COMPUTE_INITIAL_FIT)) return;
			Rf_error("Not implemented yet");
		}

		omxFitFunctionCompute(fitMat->fitFunction, FF_COMPUTE_FIT, fc);
		double fit = totalLogLikelihood(fitMat);
		omxResizeMatrix(fitMat, 1, 1);

		if (!std::isfinite(fit)) {
			fc->recordIterationError("Confidence interval is in a range that is currently incalculable. Add constraints to keep the value in the region where it can be calculated.");
			fitMat->data[0] = nan("infeasible");
			return;
		}

		double cval = 0.0;
		Eigen::Array<double,1,1> v1;
		computeConstraint(fc, fitMat, fit, v1);
		if (fabs(v1(0)) > 100) fit = nan("infeasible");
		if (!constrained) {
			cval = v1(0) * v1(0);
		}

		fitMat->data[0] = fit + cval;
	}

	virtual Diagnostic getDiag()
	{
		Diagnostic diag = DIAG_SUCCESS;
		if (fabs(eq(0)) > 1e-3) {
			diag = DIAG_BOUND_INFEASIBLE;
		}
		return diag;
	}
};

struct boundAwayCIobj : CIobjective {
	double logAlpha, sqrtCrit;
	double unboundedLL, bestLL;
	int lower;
	bool constrained;
	Eigen::Array<double, 3, 1> ineq;

	virtual bool gradientKnown()
	{
		return CI->varIndex >= 0 && constrained;
	}

	virtual void gradient(FitContext *fc, double *gradOut) {
		Eigen::Map< Eigen::VectorXd > Egrad(gradOut, fc->numParam);
		Egrad.setZero();
		Egrad[ CI->varIndex ] = lower? 1 : -1;
	}

	template <typename T1>
	void computeConstraint(double fit, Eigen::ArrayBase<T1> &v1)
	{
		double d1 = sqrt(std::max(fit - bestLL, 0.0));
		double d2 = sqrt(std::max(fit - unboundedLL,0.0));
		//Rf_pnorm(d1,0,1,0,0) + Rf_pnorm(d2,0,1,0,0) > alpha
		//d1 < sqrtCrit
		//d2 > sqrtCrit
		double pA = Rf_pnorm5(d1,0,1,0,0) + Rf_pnorm5(d2,0,1,0,0);
		v1 << std::max(d1 - sqrtCrit, 0.0), std::max(sqrtCrit - d2, 0.0),
			std::max(logAlpha - log(pA), 0.0);
		ineq = v1;
		//mxPrintMat("v1", v1);
		//mxLog("fit %g sqrtCrit %g d1 %g d2 %g alpha %g pA %g",
		//fit, sqrtCrit, d1, d2, exp(logAlpha), pA);
	}

	virtual void evalIneq(FitContext *fc, omxMatrix *fitMat, double *out)
	{
		omxFitFunctionCompute(fitMat->fitFunction, FF_COMPUTE_FIT, fc);
		const double fit = totalLogLikelihood(fitMat);
		Eigen::Map< Eigen::Array<double,3,1> >v1(out);
		computeConstraint(fit, v1);
	}

	virtual void evalFit(omxFitFunction *ff, int want, FitContext *fc)
	{
		omxMatrix *fitMat = ff->matrix;

		if (!(want & FF_COMPUTE_FIT)) {
			if (want & (FF_COMPUTE_PREOPTIMIZE | FF_COMPUTE_INITIAL_FIT)) return;
			Rf_error("Not implemented yet");
		}

		omxFitFunctionCompute(fitMat->fitFunction, FF_COMPUTE_FIT, fc);
		const double fit = totalLogLikelihood(fitMat);
		omxMatrix *ciMatrix = CI->getMatrix(fitMat->currentState);
		omxRecompute(ciMatrix, fc);
		double CIElement = omxMatrixElement(ciMatrix, CI->row, CI->col);
		omxResizeMatrix(fitMat, 1, 1);

		if (!std::isfinite(fit)) {
			fc->recordIterationError("Confidence interval is in a range that is currently incalculable. Add constraints to keep the value in the region where it can be calculated.");
			fitMat->data[0] = nan("infeasible");
			return;
		}

		double param = (lower? CIElement : -CIElement);
		double cval = 0.0;
		Eigen::Array<double,3,1> v1;
		computeConstraint(fit, v1);
		if ((v1 > 10).any()) param = nan("infeasible");
		if (!constrained) {
			cval = v1.sum()*v1.sum();
		}
			     
		fitMat->data[0] = param + cval;
		//mxLog("param at %f", fitMat->data[0]);
	}

	virtual Diagnostic getDiag()
	{
		Diagnostic diag = DIAG_SUCCESS;
		if (ineq[0] > 1e-3) {
			diag = DIAG_BA_D1;
		} else if (ineq[1] > 1e-3) {
			diag = DIAG_BA_D2;
		} else if (ineq[2] > 1e-1) {
			diag = DIAG_ALPHA_LEVEL;
		}
		return diag;
	}
};

struct boundNearCIobj : CIobjective {
	double d0, logAlpha;
	double boundLL, bestLL;
	int lower;
	bool constrained;
	Eigen::Array<double,3,1> ineq;
	double pN;
	double lbd, ubd;

	virtual bool gradientKnown()
	{
		return CI->varIndex >= 0 && constrained;
	}

	virtual void gradient(FitContext *fc, double *gradOut) {
		Eigen::Map< Eigen::VectorXd > Egrad(gradOut, fc->numParam);
		Egrad.setZero();
		Egrad[ CI->varIndex ] = lower? 1 : -1;
	}

	template <typename T1>
	void computeConstraint(double fit, Eigen::ArrayBase<T1> &v1)
	{
		double dd = sqrt(std::max(fit - bestLL, 0.0));
		double pN1 = Rf_pnorm5(dd,0,1,0,0);
		double pN2 = Rf_pnorm5((d0-dd)/2.0+dd*dd/(2*std::max(d0-dd,.001*dd*dd)),0,1,0,0);
		pN = pN1 + pN2;
		// pN > alpha
		// lbd < dd < ubd
		v1 << std::max(lbd - dd, 0.0), std::max(dd - ubd, 0.0), std::max(logAlpha - log(pN), 0.0);
		ineq = v1;
		// mxLog("pN1 %.6g pN2 %.6g d0-dd %.5g dd*dd %.5g expr %.5g",
		//       pN1, pN2, d0-dd, dd*dd, dd*dd/(2*std::max(d0-dd,.001*dd*dd)));
		// mxPrintMat("v1", v1);
		// mxLog("fit %g dd %g/%g/%g alpha %g pN %.6g",
		//       fit, lbd, dd, ubd, alpha, pN);
	}

	virtual void evalIneq(FitContext *fc, omxMatrix *fitMat, double *out)
	{
		omxFitFunctionCompute(fitMat->fitFunction, FF_COMPUTE_FIT, fc);
		const double fit = totalLogLikelihood(fitMat);
		Eigen::Map< Eigen::Array<double,3,1> > v1(out);
		computeConstraint(fit, v1);
	}

	virtual void evalFit(omxFitFunction *ff, int want, FitContext *fc)
	{
		omxMatrix *fitMat = ff->matrix;

		if (!(want & FF_COMPUTE_FIT)) {
			if (want & (FF_COMPUTE_PREOPTIMIZE | FF_COMPUTE_INITIAL_FIT)) return;
			Rf_error("Not implemented yet");
		}

		omxFitFunctionCompute(fitMat->fitFunction, FF_COMPUTE_FIT, fc);
		const double fit = totalLogLikelihood(fitMat);
		omxMatrix *ciMatrix = CI->getMatrix(fitMat->currentState);
		omxRecompute(ciMatrix, fc);
		double CIElement = omxMatrixElement(ciMatrix, CI->row, CI->col);
		omxResizeMatrix(fitMat, 1, 1);

		if (!std::isfinite(fit) || !std::isfinite(CIElement)) {
			fc->recordIterationError("Confidence interval is in a range that is currently incalculable. Add constraints to keep the value in the region where it can be calculated.");
			fitMat->data[0] = nan("infeasible");
			return;
		}

		double param = (lower? CIElement : -CIElement);
		double cval = 0.0;
		Eigen::Array<double,3,1> v1;
		computeConstraint(fit, v1);
		if ((v1 > 10).any()) param = nan("infeasible");
		if (!constrained) {
			cval = v1.sum() * v1.sum();
		}

		fitMat->data[0] = param  + cval;
		//mxLog("param at %f", fitMat->data[0]);
	}

	virtual Diagnostic getDiag()
	{
		Diagnostic diag = DIAG_SUCCESS;
		if (ineq[0] > 1e-3) {
			diag = DIAG_BN_D1;
		} else if (ineq[1] > 1e-2) {
			diag = DIAG_BN_D2;
		} else if (fabs(pN - exp(logAlpha)) > 1e-3) {
			diag = DIAG_ALPHA_LEVEL;
		}
		return diag;
	}
};

void ComputeCI::boundAdjCI(FitContext *mle, FitContext &fc, ConfidenceInterval *currentCI, int &detailRow)
{
	omxState *state = fitMatrix->currentState;
	omxMatrix *ciMatrix = currentCI->getMatrix(state);
	std::string &matName = ciMatrix->nameStr;
	omxFreeVar *fv = mle->varGroup->vars[currentCI->varIndex];
	double &nearBox = fv->lbound == NEG_INF? fv->ubound : fv->lbound;
	double &farBox = fv->lbound == NEG_INF? fv->lbound : fv->ubound;
	int side = fv->lbound == NEG_INF? ConfidenceInterval::Upper : ConfidenceInterval::Lower;

	Eigen::Map< Eigen::VectorXd > Mle(mle->est, mle->numParam);
	Eigen::Map< Eigen::VectorXd > Est(fc.est, fc.numParam);

	bool boundActive = fabs(Mle[currentCI->varIndex] - nearBox) < sqrt(std::numeric_limits<double>::epsilon());
	if (currentCI->bound[!side]) {	// ------------------------------ away from bound side --
		if (!boundActive) {
			Diagnostic diag;
			double val;
			regularCI(mle, fc, currentCI, side, val, diag);
			recordCI(NEALE_MILLER_1997, currentCI, side, fc, detailRow, val, diag);
			goto part2;
		} else {
			Global->checkpointMessage(mle, mle->est, "%s[%d, %d] unbounded fit",
						  matName.c_str(), currentCI->row + 1, currentCI->col + 1);
			double boxSave = nearBox;
			nearBox = NA_REAL;
			Est = Mle;
			plan->compute(&fc);
			nearBox = boxSave;
			if (verbose >= 2) {
				omxRecompute(ciMatrix, &fc);
				double val = omxMatrixElement(ciMatrix, currentCI->row, currentCI->col);
				mxLog("without bound, fit %.8g val %.8g", fc.fit, val);
			}
		}

		double unboundedLL = fc.fit;
		//mxLog("unboundedLL %g bestLL %g", unboundedLL, mle->fit);

		Global->checkpointMessage(mle, mle->est, "%s[%d, %d] adjusted away CI",
					  matName.c_str(), currentCI->row + 1, currentCI->col + 1);
		bool useConstr = useInequality;
		ciConstraintIneq constr(3);
		constr.fitMat = fitMatrix;
		if (useConstr) constr.push(state);
		boundAwayCIobj baobj;
		baobj.CI = currentCI;
		baobj.unboundedLL = unboundedLL;
		baobj.bestLL = mle->fit;
		baobj.logAlpha = log(1.0 - Rf_pchisq(currentCI->bound[!side], 1, 1, 0));
		baobj.sqrtCrit = sqrt(currentCI->bound[!side]);
		baobj.lower = side;
		baobj.constrained = useConstr;
		// Could set farBox to the regular UB95, but we'd need to optimize to get it
		Est = Mle;
		fc.ciobj = &baobj;
		plan->compute(&fc);
		constr.pop();

		omxRecompute(ciMatrix, &fc);
		double val = omxMatrixElement(ciMatrix, currentCI->row, currentCI->col);

		fc.ciobj = 0;
		ComputeFit(name, fitMatrix, FF_COMPUTE_FIT, &fc);
		Diagnostic diag = baobj.getDiag();
		checkOtherBoxConstraints(fc, currentCI, diag);
		recordCI(WU_NEALE_2012, currentCI, side, fc, detailRow, val, diag);
	}

 part2:
	if (currentCI->bound[side]) {     // ------------------------------ near to bound side --
		double boundLL = NA_REAL;
		double sqrtCrit95 = sqrt(currentCI->bound[side]);
		if (!boundActive) {
			Global->checkpointMessage(mle, mle->est, "%s[%d, %d] at-bound CI",
						  matName.c_str(), currentCI->row + 1, currentCI->col + 1);
			Est = Mle;
			Est[currentCI->varIndex] = nearBox; // might be infeasible
			fc.profiledOut[currentCI->varIndex] = true;
			plan->compute(&fc);
			fc.profiledOut[currentCI->varIndex] = false;
			if (fc.getInform() == 0) {
				boundLL = fc.fit;
			}

			if (verbose >= 2) {
				mxLog("%s[%d, %d]=%.2f (at bound) fit %.2f status %d",
				      matName.c_str(), currentCI->row + 1, currentCI->col + 1,
				      nearBox, fc.fit, fc.getInform());
			}
			if (!std::isfinite(boundLL)) {
				// Might work if simple approach failed
				ciConstraintEq constr(1);
				constr.fitMat = fitMatrix;
				constr.push(state);
				bound1CIobj ciobj;
				ciobj.constrained = useInequality;
				ciobj.CI = currentCI;
				ciobj.bound = nearBox;
				fc.ciobj = &ciobj;
				Est = Mle;
				plan->compute(&fc);
				constr.pop();
				boundLL = fc.fit;
				Diagnostic diag = ciobj.getDiag();
				if (diag != CIobjective::DIAG_SUCCESS) {
					recordCI(WU_NEALE_2012, currentCI, !side, fc, detailRow,
						 NA_REAL, diag);
					return;
				}
				//mxLog("got2 %.4g", fc.fit);
			}
			//mxPrintMat("bound1", ciobj.ineq);
			//mxLog("mle %g boundLL %g", mle->fit, boundLL);
		} else {
			boundLL = mle->fit;
		}

		Global->checkpointMessage(mle, mle->est, "%s[%d, %d] near side CI",
					  matName.c_str(), currentCI->row + 1, currentCI->col + 1);
		double sqrtCrit90 = sqrt(Rf_qchisq(1-(2.0 * (1-Rf_pchisq(currentCI->bound[side], 1, 1, 0))),1,1,0));
		double d0 = sqrt(std::max(boundLL - mle->fit, 0.0));
		//mxLog("d0 %.4g sqrtCrit90 %.4g d0/2 %.4g", d0, sqrtCrit90, d0/2.0);
		if (d0 < sqrtCrit90) {
			fc.fit = boundLL;
			Diagnostic diag = CIobjective::DIAG_SUCCESS;
			checkOtherBoxConstraints(fc, currentCI, diag);
			recordCI(WU_NEALE_2012, currentCI, !side, fc, detailRow, nearBox, diag);
			return;
		}
	
		if (sqrtCrit95 <= d0/2.0) {
			Diagnostic better;
			double val;
			regularCI(mle, fc, currentCI, !side, val, better);
			recordCI(NEALE_MILLER_1997, currentCI, !side, fc, detailRow, val, better);
			return;
		}

		double alphalevel = 1.0 - Rf_pchisq(currentCI->bound[side], 1, 1, 0);
		bool useConstr = useInequality;
		ciConstraintIneq constr(3);
		constr.fitMat = fitMatrix;
		if (useConstr) constr.push(state);
		boundNearCIobj bnobj;
		bnobj.CI = currentCI;
		bnobj.d0 = d0;
		bnobj.lbd = std::max(d0/2, sqrtCrit90);
		bnobj.ubd = std::min(d0, sqrtCrit95);
		bnobj.boundLL = boundLL;
		bnobj.bestLL = mle->fit;
		bnobj.logAlpha = log(alphalevel);
		bnobj.lower = !side;
		bnobj.constrained = useConstr;
		double boxSave = farBox;
		farBox = Mle[currentCI->varIndex];
		Est = Mle;
		// Perspective helps? Optimizer seems to like to start further away
		Est[currentCI->varIndex] = (9*Mle[currentCI->varIndex] + nearBox) / 10.0;
		fc.ciobj = &bnobj;
		plan->compute(&fc);
		farBox = boxSave;
		constr.pop();

		omxRecompute(ciMatrix, &fc);
		double val = omxMatrixElement(ciMatrix, currentCI->row, currentCI->col);

		//mxLog("val=%g", val);
		//mxPrintMat("bn", bnobj.ineq);
		fc.ciobj = 0;
		ComputeFit(name, fitMatrix, FF_COMPUTE_FIT, &fc);
		Diagnostic diag = bnobj.getDiag();
		checkOtherBoxConstraints(fc, currentCI, diag);
		recordCI(WU_NEALE_2012, currentCI, !side, fc, detailRow, val, diag);
	}
}

void ComputeCI::checkOtherBoxConstraints(FitContext &fc, ConfidenceInterval *currentCI,
					 Diagnostic &diag)
{
	if (diag != CIobjective::DIAG_SUCCESS) return;
	double eps = sqrt(std::numeric_limits<double>::epsilon());
	Eigen::Map< Eigen::VectorXd > Est(fc.est, fc.numParam);
	for(int px = 0; px < int(fc.numParam); px++) {
		if (px == currentCI->varIndex) continue;
		bool active=false;
		if (fabs(Est[px] - fc.varGroup->vars[px]->lbound) < eps) {
			if (verbose >= 2)
				mxLog("Param %s at lbound %f", fc.varGroup->vars[px]->name, Est[px]);
			active=true;
		}
		if (fabs(Est[px] - fc.varGroup->vars[px]->ubound) < eps) {
			if (verbose >= 2)
				mxLog("Param %s at ubound %f", fc.varGroup->vars[px]->name, Est[px]);
			active=true;
		}
		if (active) {
			diag = CIobjective::DIAG_BOXED;
			break;
		}
	}
}

void ComputeCI::regularCI(FitContext *mle, FitContext &fc, ConfidenceInterval *currentCI, int lower,
			  double &val, Diagnostic &diag)
{
	omxState *state = fitMatrix->currentState;

	ciConstraintIneq constr(1);
	bool constrained = useInequality;
	if (constrained) {
		constr.fitMat = fitMatrix;
		constr.push(state);
	}

	// Reset to previous optimum
	Eigen::Map< Eigen::VectorXd > Mle(mle->est, mle->numParam);
	Eigen::Map< Eigen::VectorXd > Est(fc.est, fc.numParam);
	Est = Mle;

	regularCIobj ciobj;
	ciobj.CI = currentCI;
	ciobj.compositeCIFunction = !constrained;
	ciobj.lowerBound = lower;
	ciobj.targetFit = currentCI->bound[!lower] + mle->fit;
	fc.ciobj = &ciobj;
	//mxLog("Set target fit to %f (MLE %f)", fc->targetFit, fc->fit);

	plan->compute(&fc);
	constr.pop();

	omxMatrix *ciMatrix = currentCI->getMatrix(fitMatrix->currentState);
	omxRecompute(ciMatrix, &fc);
	val = omxMatrixElement(ciMatrix, currentCI->row, currentCI->col);

	// We check the fit again so we can report it
	// in the detail data.frame.
	fc.ciobj = 0;
	ComputeFit(name, fitMatrix, FF_COMPUTE_FIT, &fc);

	diag = ciobj.getDiag();

	if (diag == CIobjective::DIAG_SUCCESS) {
		double eps = sqrt(std::numeric_limits<double>::epsilon());
		for(int px = 0; px < int(fc.numParam); px++) {
			bool active=false;
			if (fabs(Est[px] - fc.varGroup->vars[px]->lbound) < eps) {
				if (verbose >= 2)
					mxLog("Param %s at lbound %f", fc.varGroup->vars[px]->name, Est[px]);
				active=true;
			}
			if (fabs(Est[px] - fc.varGroup->vars[px]->ubound) < eps) {
				if (verbose >= 2)
					mxLog("Param %s at ubound %f", fc.varGroup->vars[px]->name, Est[px]);
				active=true;
			}
			if (active) {
				diag = CIobjective::DIAG_BOXED;
				break;
			}
		}
	}
}

void ComputeCI::regularCI2(FitContext *mle, FitContext &fc, ConfidenceInterval *currentCI, int &detailRow)
{
	omxState *state = fitMatrix->currentState;
	omxMatrix *ciMatrix = currentCI->getMatrix(state);
	std::string &matName = ciMatrix->nameStr;

	for (int upper=0; upper <= 1; ++upper) {
		int lower = 1-upper;
		if (!(currentCI->bound[upper])) continue;

		Global->checkpointMessage(mle, mle->est, "%s[%d, %d] %s CI",
					  matName.c_str(), currentCI->row + 1, currentCI->col + 1,
					  upper? "upper" : "lower");
		double val;
		Diagnostic better;
		regularCI(mle, fc, currentCI, lower, val, better);
		recordCI(NEALE_MILLER_1997, currentCI, lower, fc, detailRow, val, better);
	}
}

void ComputeCI::computeImpl(FitContext *mle)
{
	if (intervals) Rf_error("Can only compute CIs once");
	if (!Global->intervals) {
		if (verbose >= 1) mxLog(name, "%s: mxRun(..., intervals=FALSE), skipping");
		return;
	}

	Global->unpackConfidenceIntervals(mle->state);

	// Not strictly necessary, but makes it easier to run
	// mxComputeConfidenceInterval alone without other compute
	// steps.
	omxAlgebraPreeval(fitMatrix, mle);
	ComputeFit(name, fitMatrix, FF_COMPUTE_FIT, mle);

	int numInts = (int) Global->intervalList.size();
	if (verbose >= 1) mxLog("%s: %d intervals of '%s' (ref fit %f %s)",
				name, numInts, fitMatrix->name(), mle->fit, ctypeName);
	if (!numInts) return;

	if (!mle->haveReferenceFit(fitMatrix)) return;

	// I'm not sure why INFORM_NOT_AT_OPTIMUM is okay, but that's how it was.
	if (mle->getInform() >= INFORM_LINEAR_CONSTRAINTS_INFEASIBLE && mle->getInform() != INFORM_NOT_AT_OPTIMUM) {
		// TODO: allow forcing
		Rf_warning("Not calculating confidence intervals because of optimizer status %d", mle->getInform());
		return;
	}

	Rf_protect(intervals = Rf_allocMatrix(REALSXP, numInts, 3));
	Rf_protect(intervalCodes = Rf_allocMatrix(INTSXP, numInts, 2));

	int totalIntervals = 0;
	for(int j = 0; j < numInts; j++) {
		ConfidenceInterval *oCI = Global->intervalList[j];
		totalIntervals += (oCI->bound != 0.0).count();
	}

	int numDetailCols = 7 + mle->numParam;
	Rf_protect(detail = Rf_allocVector(VECSXP, numDetailCols));
	SET_VECTOR_ELT(detail, 0, Rf_allocVector(STRSXP, totalIntervals));
	SET_VECTOR_ELT(detail, 1, Rf_allocVector(REALSXP, totalIntervals));
	const char *sideLabels[] = { "upper", "lower" };
	SET_VECTOR_ELT(detail, 2, makeFactor(Rf_allocVector(INTSXP, totalIntervals), 2, sideLabels));
	for (int cx=0; cx < 1+int(mle->numParam); ++cx) {
		SET_VECTOR_ELT(detail, 3+cx, Rf_allocVector(REALSXP, totalIntervals));
	}
	const char *methodLabels[] = { "neale-miller-1997", "wu-neale-2012" };
	SET_VECTOR_ELT(detail, 4+mle->numParam,
		       makeFactor(Rf_allocVector(INTSXP, totalIntervals),
				  OMX_STATIC_ARRAY_SIZE(methodLabels), methodLabels));
	const char *diagLabels[] = {
		"success", "alpha level not reached",
		"bound-away mle distance", "bound-away unbounded distance",
		"bound-near lower distance", "bound-near upper distance",
		"bound infeasible", "active box constraint"
	};
	SET_VECTOR_ELT(detail, 5+mle->numParam,
		       makeFactor(Rf_allocVector(INTSXP, totalIntervals),
				  OMX_STATIC_ARRAY_SIZE(diagLabels), diagLabels));
	const char *statusCodeLabels[] = { // see ComputeInform type
		"OK", "OK/green",
		"infeasible linear constraint",
		"infeasible non-linear constraint",
		"iteration limit",
		"not convex",
		"nonzero gradient",
		"bad deriv",
		"?",
		"internal error",
		"infeasible start"
	};
	SET_VECTOR_ELT(detail, 6+mle->numParam,
		       makeFactor(Rf_allocVector(INTSXP, totalIntervals),
				  OMX_STATIC_ARRAY_SIZE(statusCodeLabels), statusCodeLabels));

	SEXP detailCols;
	Rf_protect(detailCols = Rf_allocVector(STRSXP, numDetailCols));
	Rf_setAttrib(detail, R_NamesSymbol, detailCols);
	SET_STRING_ELT(detailCols, 0, Rf_mkChar("parameter"));
	SET_STRING_ELT(detailCols, 1, Rf_mkChar("value"));
	SET_STRING_ELT(detailCols, 2, Rf_mkChar("side"));
	SET_STRING_ELT(detailCols, 3, Rf_mkChar("fit"));
	for (int nx=0; nx < int(mle->numParam); ++nx) {
		SET_STRING_ELT(detailCols, 4+nx, Rf_mkChar(mle->varGroup->vars[nx]->name));
	}
	SET_STRING_ELT(detailCols, 4 + mle->numParam, Rf_mkChar("method"));
	SET_STRING_ELT(detailCols, 5 + mle->numParam, Rf_mkChar("diagnostic"));
	SET_STRING_ELT(detailCols, 6 + mle->numParam, Rf_mkChar("statusCode"));

	markAsDataFrame(detail, totalIntervals);

	omxState *state = fitMatrix->currentState;
	FitContext fc(mle, mle->varGroup);
	FreeVarGroup *freeVarGroup = fc.varGroup;

	int detailRow = 0;
	for(int ii = 0; ii < (int) Global->intervalList.size(); ii++) {
		ConfidenceInterval *currentCI = Global->intervalList[ii];
		omxMatrix *ciMatrix = currentCI->getMatrix(state);

		currentCI->varIndex = freeVarGroup->lookupVar(ciMatrix, currentCI->row, currentCI->col);

		if (currentCI->boundAdj && currentCI->varIndex < 0) {
			currentCI->boundAdj = false;
		}
		if (currentCI->boundAdj) {
			omxFreeVar *fv = freeVarGroup->vars[currentCI->varIndex];
			if (!((fv->lbound == NEG_INF) ^ (fv->ubound == INF))) {
				currentCI->boundAdj = false;
			}
		}
		if (currentCI->boundAdj) {
			boundAdjCI(mle, fc, currentCI, detailRow);
		} else {
			regularCI2(mle, fc, currentCI, detailRow);
		}
	}

	mle->copyParamToModel();

	Eigen::Map< Eigen::ArrayXXd > interval(REAL(intervals), numInts, 3);
	interval.fill(NA_REAL);
	int* intervalCode = INTEGER(intervalCodes);
	for(int j = 0; j < numInts; j++) {
		ConfidenceInterval *oCI = Global->intervalList[j];
		omxMatrix *ciMat = oCI->getMatrix(state);
		omxRecompute(ciMat, mle);
		interval(j, 1) = omxMatrixElement(ciMat, oCI->row, oCI->col);
		interval(j, 0) = std::min(oCI->val[ConfidenceInterval::Lower], interval(j, 1));
		interval(j, 2) = std::max(oCI->val[ConfidenceInterval::Upper], interval(j, 1));
		intervalCode[j] = oCI->code[ConfidenceInterval::Lower];
		intervalCode[j + numInts] = oCI->code[ConfidenceInterval::Upper];
	}
}

void ComputeCI::collectResults(FitContext *fc, LocalComputeResult *lcr, MxRList *out)
{
	super::collectResults(fc, lcr, out);
	std::vector< omxCompute* > clist(1);
	clist[0] = plan;
	collectResultsHelper(fc, clist, lcr, out);
}

void ComputeCI::reportResults(FitContext *fc, MxRList *slots, MxRList *out)
{
	if (!intervals) return;

	int numInt = (int) Global->intervalList.size();

	SEXP dimnames;
	SEXP names;
	Rf_protect(dimnames = Rf_allocVector(VECSXP, 2));
	Rf_protect(names = Rf_allocVector(STRSXP, 3));
	SET_STRING_ELT(names, 0, Rf_mkChar("lbound"));
	SET_STRING_ELT(names, 1, Rf_mkChar("estimate"));
	SET_STRING_ELT(names, 2, Rf_mkChar("ubound"));
	SET_VECTOR_ELT(dimnames, 1, names);

	Rf_protect(names = Rf_allocVector(STRSXP, numInt)); //shared between the two matrices
	for (int nx=0; nx < numInt; ++nx) {
		ConfidenceInterval *ci = Global->intervalList[nx];
		SET_STRING_ELT(names, nx, Rf_mkChar(ci->name.c_str()));
	}
	SET_VECTOR_ELT(dimnames, 0, names);

	Rf_setAttrib(intervals, R_DimNamesSymbol, dimnames);

	out->add("confidenceIntervals", intervals);

	Rf_protect(dimnames = Rf_allocVector(VECSXP, 2));
	SET_VECTOR_ELT(dimnames, 0, names);

	Rf_protect(names = Rf_allocVector(STRSXP, 2));
	SET_STRING_ELT(names, 0, Rf_mkChar("lbound"));
	SET_STRING_ELT(names, 1, Rf_mkChar("ubound"));
	SET_VECTOR_ELT(dimnames, 1, names);

	Rf_setAttrib(intervalCodes, R_DimNamesSymbol, dimnames);

	out->add("confidenceIntervalCodes", intervalCodes);

	MxRList output;
	output.add("detail", detail);
	slots->add("output", output.asR());
}

// ---------------------------------------------------------------

class ComputeTryH : public omxCompute {
	typedef omxCompute super;
	omxCompute *plan;
	int verbose;
	double loc;
	double scale;
	int maxRetries;
	int invocations;
	int numRetries;
	Eigen::ArrayXd bestEst;
	int bestStatus;
	double bestFit;

	static bool satisfied(FitContext *fc);
public:
	//ComputeTryH();
	virtual void initFromFrontend(omxState *, SEXP rObj);
	virtual void computeImpl(FitContext *fc);
	virtual void reportResults(FitContext *fc, MxRList *slots, MxRList *out);
	virtual void collectResults(FitContext *fc, LocalComputeResult *lcr, MxRList *out);
};

omxCompute *newComputeTryHard()
{ return new ComputeTryH(); }

void ComputeTryH::initFromFrontend(omxState *globalState, SEXP rObj)
{
	super::initFromFrontend(globalState, rObj);

	{
		ProtectedSEXP Rverbose(R_do_slot(rObj, Rf_install("verbose")));
		verbose = Rf_asInteger(Rverbose);

		ProtectedSEXP Rloc(R_do_slot(rObj, Rf_install("location")));
		loc = Rf_asReal(Rloc);
		ProtectedSEXP Rscale(R_do_slot(rObj, Rf_install("scale")));
		scale = Rf_asReal(Rscale);
		ProtectedSEXP Rretries(R_do_slot(rObj, Rf_install("maxRetries")));
		maxRetries = Rf_asReal(Rretries);
	}

	invocations = 0;
	numRetries = 0;

	SEXP slotValue;
	Rf_protect(slotValue = R_do_slot(rObj, Rf_install("plan")));
	SEXP s4class;
	Rf_protect(s4class = STRING_ELT(Rf_getAttrib(slotValue, R_ClassSymbol), 0));
	plan = omxNewCompute(globalState, CHAR(s4class));
	plan->initFromFrontend(globalState, slotValue);
}

bool ComputeTryH::satisfied(FitContext *fc)
{
	return (fc->getInform() == INFORM_CONVERGED_OPTIMUM ||
		fc->getInform() == INFORM_UNCONVERGED_OPTIMUM ||
		fc->getInform() == INFORM_ITERATION_LIMIT);
}

void ComputeTryH::computeImpl(FitContext *fc)
{
	using Eigen::Map;
	using Eigen::ArrayXd;
	Map< ArrayXd > curEst(fc->est, fc->numParam);
	ArrayXd origStart = curEst;
	bestEst = curEst;

	++invocations;

	GetRNGstate();

	// return record of attempted starting vectors? TODO

	int retriesRemain = maxRetries - 1;
	if (verbose >= 1) {
		mxLog("%s: at most %d attempts (Welcome)", name, retriesRemain);
	}

	bestStatus = INFORM_UNINITIALIZED;
	bestFit = NA_REAL;
	fc->setInform(INFORM_UNINITIALIZED);
	plan->compute(fc);
	if (fc->getInform() != INFORM_UNINITIALIZED && fc->getInform() != INFORM_STARTING_VALUES_INFEASIBLE) {
		bestStatus = fc->getInform();
		bestEst = curEst;
		bestFit = fc->fit;
	}

	while (!satisfied(fc) && retriesRemain > 0) {
		if (verbose >= 2) {
			mxLog("%s: fit %.2f inform %d, %d retries remain", name, fc->fit,
			      fc->getInform(), retriesRemain);
		}

		curEst = origStart;
		for (int vx=0; vx < curEst.size(); ++vx) {
			double adj1 = loc + unif_rand() * 2.0 * scale - scale;
			double adj2 = 0.0 + unif_rand() * 2.0 * scale - scale;
			if (verbose >= 3) {
				mxLog("%d %g %g", vx, adj1, adj2);
			}
			curEst[vx] = curEst[vx] * adj1 + adj2;
		}

		--retriesRemain;

		fc->setInform(INFORM_UNINITIALIZED);
		plan->compute(fc);
		if (fc->getInform() != INFORM_UNINITIALIZED && fc->getInform() != INFORM_STARTING_VALUES_INFEASIBLE &&
		    (bestStatus == INFORM_UNINITIALIZED || fc->getInform() < bestStatus)) {
			bestStatus = fc->getInform();
			bestEst = curEst;
			bestFit = fc->fit;
		}
	}

	fc->setInform(bestStatus);
	curEst = bestEst;
	fc->fit = bestFit;

	numRetries += maxRetries - retriesRemain;
	if (verbose >= 1) {
		mxLog("%s: fit %.2f inform %d after %d attempt(s)", name, fc->fit, fc->getInform(),
		      maxRetries - retriesRemain);
	}

	PutRNGstate();
}

void ComputeTryH::collectResults(FitContext *fc, LocalComputeResult *lcr, MxRList *out)
{
	super::collectResults(fc, lcr, out);
	std::vector< omxCompute* > clist(1);
	clist[0] = plan;
	collectResultsHelper(fc, clist, lcr, out);
}

void ComputeTryH::reportResults(FitContext *fc, MxRList *slots, MxRList *out)
{
	MxRList info;
	info.add("invocations", Rf_ScalarInteger(invocations));
	info.add("retries", Rf_ScalarInteger(numRetries));
	slots->add("debug", info.asR());
}
