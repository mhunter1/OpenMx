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

#include "omxState.h"
#include "omxMatrix.h"
#include "glue.h"
#include "omxCsolnp.h"

#ifdef SHADOW_DIAG
#pragma GCC diagnostic warning "-Wshadow"
#endif

void omxCSOLNP(GradientOptimizerContext &go)
{
	double *est = go.est.data();
	go.optName = "CSOLNP";
	if (!std::isfinite(go.ControlTolerance)) go.ControlTolerance = 1e-9;
	go.useGradient = false;  // not implemented yet
	solnp(est, go);
}

