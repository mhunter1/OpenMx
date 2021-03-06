#
#   Copyright 2007-2017 The OpenMx Project
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
# 
#        http://www.apache.org/licenses/LICENSE-2.0
# 
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.


require(OpenMx)

foo <- mxMatrix('Full', 1, 1, free = TRUE, name = 'foo', dimnames=list('a','b'))
bar <- mxAlgebra(cbind(foo, foo), name = 'bar')
baz <- mxAlgebra((foo[1,1] - 2) %*% (foo[1,1] - 2), name = 'baz')
objective <- mxFitFunctionAlgebra('baz')
model <- mxModel('model', foo, bar, baz, objective,
		 mxMatrix('Full', 2, 2, values=.5, name="half"),
		 mxAlgebra(half * foo, name="halfFoo"))
out <- mxRun(model, suppressWarnings=TRUE)
outSummary <- summary(out)
omxCheckEquals(nrow(outSummary$parameters), 1)

omxCheckTrue(all(dim(out$foo$values) == 1))
omxCheckEquals(rownames(out$foo), "a")
omxCheckEquals(colnames(out$foo), "b")
omxCheckTrue(all(dim(out$halfFoo) == 2))
