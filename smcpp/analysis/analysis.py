import numpy as np
import json
import sys

from .. import estimation_tools, _smcpp, util, logging, spline, data_filter, beta_de
from ..model import SMCModel
from . import base
import smcpp.defaults
from smcpp.optimize.optimizers import SMCPPOptimizer
from smcpp.optimize.plugins import analysis_saver, parameter_optimizer

logger = logging.getLogger(__name__)


class Analysis(base.BaseAnalysis):
    """A dataset, model and inference manager to be used for estimation."""

    def __init__(self, files, args):
        super().__init__(files, args)

        pipe = self._pipeline
        pipe.add_filter(data_filter.Thin(thinning=args.thinning))
        pipe.add_filter(data_filter.BinObservations(w=args.w))
        pipe.add_filter(data_filter.RecodeMonomorphic())
        pipe.add_filter(data_filter.Compress())
        pipe.add_filter(data_filter.Validate())
        pipe.add_filter(data_filter.DropUninformativeContigs())
        pipe.add_filter(data_filter.Summarize())

        if self.npop != 1:
            logger.error("Please use 'smc++ split' to estimate two-population models")
            sys.exit(1)

        NeN0 = self._pipeline["watterson"].theta_hat / (2. * args.mu * self._N0)
        m = SMCModel([1.], self._N0, spline.Piecewise, None)
        m[:] = np.log(NeN0)
        hs = estimation_tools.balance_hidden_states(m, args.knots)
        if args.timepoints is not None:
            hs = np.geomspace(args.timepoints[0], args.timepoints[1], args.knots)
        hs /= (2 * self._N0)
        self.hidden_states = hs
        self._knots = hs[1:-1]
        logger.debug("Knots are: %s", self._knots)

        self._init_model(args.spline)
        self._init_inference_manager(args.polarization_error, self.hidden_states)
        self.alpha = args.w
        self._model[:] = np.log(NeN0)
        self._model.randomize()
        self._init_optimizer(
            args.outdir,
            args.algorithm,
            args.xtol,
            args.ftol,
            learn_rho=args.r is None,
            single=args.no_multi,
        )
        self._init_regularization(args)

    def _init_model(self, spline_class):
        ## Initialize model
        logger.debug("knots in coalescent scaling:\n%s", str(self._knots))
        spline_class = {
            "cubic": spline.CubicSpline,
            "bspline": spline.BSpline,
            "akima": spline.AkimaSpline,
            "pchip": spline.PChipSpline,
            "piecewise": spline.Piecewise,
        }[spline_class]
        assert self.npop == 1
        self._model = SMCModel(self._knots, self._N0, spline_class, self.populations[0])

    def _init_regularization(self, args):
        if self._args.lambda_:
            self._penalty = args.lambda_
        else:
            self._penalty = abs(self.Q()) * (10 ** -args.regularization_penalty)
        logger.debug("Regularization penalty: lambda=%g", self._penalty)

    _OPTIMIZER_CLS = SMCPPOptimizer

    def _init_optimizer(self, outdir, algorithm, xtol, ftol, learn_rho, single):
        super()._init_optimizer(outdir, algorithm, xtol, ftol, single)
        if learn_rho:
            rho_bounds = lambda: (self._theta / 100, 100 * self._theta)
            self._optimizer.register_plugin(
                parameter_optimizer.ParameterOptimizer("rho", rho_bounds)
            )
