/*!
* This file is part of GPBoost a C++ library for combining
*	boosting with Gaussian process and mixed effects models
*
* Copyright (c) 2020 Fabio Sigrist. All rights reserved.
*
* Licensed under the Apache License Version 2.0. See LICENSE file in the project root for license information.
*/
#ifndef GPB_LIKELIHOODS_
#define GPB_LIKELIHOODS_

#define _USE_MATH_DEFINES // for M_SQRT1_2 and M_PI
#include <cmath>

#include <GPBoost/log.h>
#include <GPBoost/type_defs.h>
#include <GPBoost/sparse_matrix_utils.h>

#include <string>
#include <set>
#include <string>
#include <vector>
#include <cmath>

//Mathematical constants usually defined in cmath
#ifndef M_PI
#define M_PI      3.1415926535897932384626433832795029
#endif
//sqrt(2)
#ifndef M_SQRT2
#define M_SQRT2      1.41421356237309504880
#endif
//1/sqrt(2)
#ifndef M_SQRT1_2
#define M_SQRT1_2      0.707106781186547524401
#endif
//2/sqrt(pi)
#ifndef M_2_SQRTPI
#define M_2_SQRTPI      1.12837916709551257390
#endif

#include <chrono>  // only needed for debugging
#include <thread> // only needed for debugging

namespace GPBoost {

	/*!
	* \brief This class implements the likelihoods for the Gaussian proceses
	* The template parameter T2 can be either <chol_sp_mat_t> or <chol_den_mat_t>
	*/
	template<typename T2>
	class Likelihood {
	public:
		/*! \brief Constructor */
		Likelihood();

		/*!
		* \brief Constructor
		* \param likelihood Type of likelihood
		*/
		Likelihood(string_t type, data_size_t num_data, data_size_t num_re) {
			string_t likelihood = ParseLikelihoodAlias(type);
			if (SUPPORTED_LIKELIHOODS_.find(likelihood) == SUPPORTED_LIKELIHOODS_.end()) {
				Log::Fatal("Likelihood of type '%s' is not supported.", likelihood.c_str());
			}
			likelihood_type_ = likelihood;
			num_data_ = num_data;
			num_re_ = num_re;
			if (likelihood_type_ == "gamma") {
				aux_pars_ = { 1. };//shape parameter, TODO: also estimate this parameter
			}
		}

		/*!
		* \brief Initialize mode vectors and auxiliary vector a_vec_ (used in Laplace approximation for non-Gaussian data)
		*/
		void InitializeModeAvec() {
			mode_ = vec_t(num_re_);
			mode_.setZero();
			mode_initialized_ = true;
			first_deriv_ll_ = vec_t(num_data_);
			second_deriv_neg_ll_ = vec_t(num_data_);
		}

		/*! \brief Destructor */
		~Likelihood() {
		}

		/*!
		* \brief Returns the type of likelihood
		*/
		string_t GetLikelihood() const {
			return(likelihood_type_);
		}

		/*!
		* \brief Set the type of likelihood
		*/
		void SetLikelihood(const string_t& type) {
			string_t likelihood = ParseLikelihoodAlias(type);
			if (SUPPORTED_LIKELIHOODS_.find(likelihood) == SUPPORTED_LIKELIHOODS_.end()) {
				Log::Fatal("Likelihood of type '%s' is not supported.", likelihood.c_str());
			}
			likelihood_type_ = likelihood;
		}

		/*!
		* \brief Returns the type of the response variable (label). Either "double" or "int"
		*/
		string_t label_type() const {
			if (likelihood_type_ == "bernoulli_probit" || likelihood_type_ == "bernoulli_logit" ||
				likelihood_type_ == "poisson") {
				return("int");
			}
			else {
				return("double");
			}
		}

		/*!
		* \brief Checks whether the response variables (labels) have the correct values
		* \param y_data Response variable data
		* \param num_data Number of data points
		*/
		template <typename T>//T can be double or float
		void CheckY(const T* y_data, const data_size_t num_data) const {
			if (likelihood_type_ == "bernoulli_probit" || likelihood_type_ == "bernoulli_logit") {
				//#pragma omp parallel for schedule(static)//problematic with error message below... 
				for (data_size_t i = 0; i < num_data; ++i) {
					if (fabs(y_data[i]) >= EPSILON_ && !AreSame<T>(y_data[i], 1.)) {
						Log::Fatal("Response variable (label) data needs to be 0 or 1 for likelihood of type '%s'.", likelihood_type_.c_str());
					}
				}
			}
			else if (likelihood_type_ == "poisson") {
				for (data_size_t i = 0; i < num_data; ++i) {
					if (y_data[i] < 0) {
						Log::Fatal("Found negative response variable. Response variable cannot be negative for likelihood of type '%s'.", likelihood_type_.c_str());
					}
					else {
						double intpart;
						if (std::modf(y_data[i], &intpart) != 0.0) {
							Log::Fatal("Found non-integer response variable. Response variable can only be integer valued for likelihood of type '%s'.", likelihood_type_.c_str());
						}
					}
				}
			}
			else if (likelihood_type_ == "gamma") {
				for (data_size_t i = 0; i < num_data; ++i) {
					if (y_data[i] < 0) {
						Log::Fatal("Found negative response variable. Response variable cannot be negative for likelihood of type '%s'.", likelihood_type_.c_str());
					}
				}
			}
		}

		/*!
		* \brief Calculate normalizing constant for (log-)likelihood calculation
		* \param y_data Response variable data
		* \param num_data Number of data points
		*/
		template <typename T>//T can be double or int
		void CalculateNormalizingConstant(const T* y_data, const data_size_t num_data) {
			if (likelihood_type_ == "poisson") {
				double log_normalizing_constant = 0.;
#pragma omp parallel for schedule(static) reduction(+:log_normalizing_constant)
				for (data_size_t i = 0; i < num_data; ++i) {
					if (y_data[i] > 1) {
						double log_factorial = 0.;
						for (int k = 2; k <= y_data[i]; ++k) {
							log_factorial += std::log(k);
						}
						log_normalizing_constant += log_factorial;
					}
				}
				log_normalizing_constant_ = log_normalizing_constant;
			}
			else if (likelihood_type_ == "gamma") {
				//				//Currently not used since aux_pars_[0]==1 and thus log_normalizing_constant_==0
				//				double log_normalizing_constant = 0.;
				//#pragma omp parallel for schedule(static) reduction(+:log_normalizing_constant)
				//				for (data_size_t i = 0; i < num_data; ++i) {
				//					log_normalizing_constant += -(aux_pars_[0] - 1.) * std::log(y_data[i]) - aux_pars_[0] * std::log(aux_pars_[0]) + std::tgamma(aux_pars_[0]);
				//				}
				//				log_normalizing_constant_ = log_normalizing_constant;
				log_normalizing_constant_ = 0. * y_data[0];//y_data[0] is just a trick to avoid compiler warnings complaning about unreferenced parameters...
			}
			normalizing_constant_has_been_calculated_ = true;
		}

		/*!
		* \brief Evaluate the log-likelihood conditional on the latent variable (=location_par)
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param location_par Location parameter (random plus fixed effects)
		* \param num_data Number of data points
		*/
		double LogLikelihood(const double* y_data, const int* y_data_int,
			const double* location_par, const data_size_t num_data) {
			if (!normalizing_constant_has_been_calculated_) {
				Log::Fatal("The normalizing constant has not been calculated. Call 'CalculateNormalizingConstant' first.");
			}
			double ll = 0.;
			if (likelihood_type_ == "bernoulli_probit") {
#pragma omp parallel for schedule(static) reduction(+:ll)
				for (data_size_t i = 0; i < num_data; ++i) {
					if (y_data_int[i] == 0) {
						ll += std::log(1 - normalCDF(location_par[i]));
					}
					else {
						ll += std::log(normalCDF(location_par[i]));
					}
				}
			}
			else if (likelihood_type_ == "bernoulli_logit") {
#pragma omp parallel for schedule(static) reduction(+:ll)
				for (data_size_t i = 0; i < num_data; ++i) {
					ll += y_data_int[i] * location_par[i] - std::log(1 + std::exp(location_par[i]));
					//Alternative version:
					//if (y_data_int[i] == 0) {
					//	ll += std::log(1 - CondMeanLikelihood(location_par[i]));//CondMeanLikelihood = logistic function
					//}
					//else {
					//	ll += std::log(CondMeanLikelihood(location_par[i]));
					//}
				}
			}
			else if (likelihood_type_ == "poisson") {
#pragma omp parallel for schedule(static) reduction(+:ll)
				for (data_size_t i = 0; i < num_data; ++i) {
					ll += y_data_int[i] * location_par[i] - std::exp(location_par[i]);
				}
				ll -= log_normalizing_constant_;
			}
			else if (likelihood_type_ == "gamma") {
#pragma omp parallel for schedule(static) reduction(+:ll)
				for (data_size_t i = 0; i < num_data; ++i) {
					ll += -aux_pars_[0] * (location_par[i] + y_data[i] * std::exp(-location_par[i]));
				}
				ll -= log_normalizing_constant_;
			}
			return(ll);
		}

		/*!
		* \brief Calculate the first derivative of the log-likelihood with respect to the location parameter
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param location_par Location parameter (random plus fixed effects)
		* \param num_data Number of data points
		*/
		void CalcFirstDerivLogLik(const double* y_data, const int* y_data_int,
			const double* location_par, const data_size_t num_data) {
			if (likelihood_type_ == "bernoulli_probit") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					if (y_data_int[i] == 0) {
						first_deriv_ll_[i] = -normalPDF(location_par[i]) / (1 - normalCDF(location_par[i]));
					}
					else {
						first_deriv_ll_[i] = normalPDF(location_par[i]) / normalCDF(location_par[i]);
					}
				}
			}
			else if (likelihood_type_ == "bernoulli_logit") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					first_deriv_ll_[i] = y_data_int[i] - CondMeanLikelihood(location_par[i]);//CondMeanLikelihood = logistic(x)
				}
			}
			else if (likelihood_type_ == "poisson") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					first_deriv_ll_[i] = y_data_int[i] - std::exp(location_par[i]);
				}
			}
			else if (likelihood_type_ == "gamma") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					first_deriv_ll_[i] = aux_pars_[0] * (y_data[i] * std::exp(-location_par[i]) - 1.);
				}
			}
		}

		/*!
		* \brief Calculate the second derivative of the negative (!) log-likelihood with respect to the location parameter
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param location_par Location parameter (random plus fixed effects)
		* \param num_data Number of data points
		*/
		void CalcSecondDerivNegLogLik(const double* y_data, const int* y_data_int,
			const double* location_par, const data_size_t num_data) {
			if (likelihood_type_ == "bernoulli_probit") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					double dnorm = normalPDF(location_par[i]);
					double pnorm = normalCDF(location_par[i]);
					if (y_data_int[i] == 0) {
						double dnorm_frac_one_min_pnorm = dnorm / (1. - pnorm);
						second_deriv_neg_ll_[i] = -dnorm_frac_one_min_pnorm * (location_par[i] - dnorm_frac_one_min_pnorm);
					}
					else {
						double dnorm_frac_pnorm = dnorm / pnorm;
						second_deriv_neg_ll_[i] = dnorm_frac_pnorm * (location_par[i] + dnorm_frac_pnorm);
					}
				}
			}
			else if (likelihood_type_ == "bernoulli_logit") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					double exp_loc_i = std::exp(location_par[i]);
					second_deriv_neg_ll_[i] = exp_loc_i * std::pow(1. + exp_loc_i, -2);
				}
			}
			else if (likelihood_type_ == "poisson") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					second_deriv_neg_ll_[i] = std::exp(location_par[i]);
				}
			}
			else if (likelihood_type_ == "gamma") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					second_deriv_neg_ll_[i] = aux_pars_[0] * y_data[i] * std::exp(-location_par[i]);
				}
			}
		}

		/*!
		* \brief Calculate the third derivative of the log-likelihood with respect to the location parameter
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param location_par Location parameter (random plus fixed effects)
		* \param num_data Number of data points
		* \param[out] third_deriv Third derivative of the log-likelihood with respect to the location parameter. Need to pre-allocate memory of size num_data
		*/
		void CalcThirdDerivLogLik(const double* y_data, const int* y_data_int,
			const double* location_par, const data_size_t num_data, double* third_deriv) {
			if (likelihood_type_ == "bernoulli_probit") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					double dnorm = normalPDF(location_par[i]);
					double pnorm = normalCDF(location_par[i]);
					if (y_data_int[i] == 0) {
						double dnorm_frac_one_min_pnorm = dnorm / (1. - pnorm);
						third_deriv[i] = dnorm_frac_one_min_pnorm * (1 - location_par[i] * location_par[i] +
							dnorm_frac_one_min_pnorm * (3 * location_par[i] - 2 * dnorm_frac_one_min_pnorm));
					}
					else {
						double dnorm_frac_pnorm = dnorm / pnorm;
						third_deriv[i] = dnorm_frac_pnorm * (location_par[i] * location_par[i] - 1 +
							dnorm_frac_pnorm * (3 * location_par[i] + 2 * dnorm_frac_pnorm));
					}
				}
			}
			else if (likelihood_type_ == "bernoulli_logit") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					double exp_loc_i = std::exp(location_par[i]);
					third_deriv[i] = -exp_loc_i * (1. - exp_loc_i) * std::pow(1 + exp_loc_i, -3);
				}
			}
			else if (likelihood_type_ == "poisson") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					third_deriv[i] = -std::exp(location_par[i]);
				}
			}
			else if (likelihood_type_ == "gamma") {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					third_deriv[i] = aux_pars_[0] * y_data[i] * std::exp(-location_par[i]);
				}
			}
		}

		/*!
		* \brief Calculate the mean of the likelihood conditional on the (predicted) latent variable
		*			Used for adaptive Gauss-Hermite quadrature for the prediction of the response variable
		*/
		inline double CondMeanLikelihood(const double value) const {
			if (likelihood_type_ == "gaussian") {
				return value;
			}
			else if (likelihood_type_ == "bernoulli_probit") {
				return normalCDF(value);
			}
			else if (likelihood_type_ == "bernoulli_logit") {
				return 1. / (1. + std::exp(-value));
			}
			else if (likelihood_type_ == "poisson") {
				return std::exp(value);
			}
			else if (likelihood_type_ == "gamma") {
				return std::exp(value);
			}
			else {
				Log::Fatal("CondMeanLikelihood: Likelihood of type '%s' is not supported.", likelihood_type_.c_str());
				return 0.;
			}
		}

		/*!
		* \brief Calculate the first derivative of the logarithm of the mean of the likelihood conditional on the (predicted) latent variable
		*			Used for adaptive Gauss-Hermite quadrature for the prediction of the response variable
		*/
		inline double FirstDerivLogCondMeanLikelihood(const double value) const {
			if (likelihood_type_ == "bernoulli_logit") {
				return 1. / (1. + std::exp(value));
			}
			else if (likelihood_type_ == "poisson") {
				return 1.;
			}
			else if (likelihood_type_ == "gamma") {
				return 1.;
			}
			else {
				Log::Fatal("FirstDerivLogCondMeanLikelihood: Likelihood of type '%s' is not supported.", likelihood_type_.c_str());
				return 0.;
			}
		}

		/*!
		* \brief Calculate the second derivative of the logarithm of the mean of the likelihood conditional on the (predicted) latent variable
		*			Used for adaptive Gauss-Hermite quadrature for the prediction of the response variable
		*/
		inline double SecondDerivLogCondMeanLikelihood(const double value) const {
			if (likelihood_type_ == "bernoulli_logit") {
				double exp_x = std::exp(value);
				return -exp_x / ((1. + exp_x) * (1. + exp_x));
			}
			else if (likelihood_type_ == "poisson") {
				return 0.;
			}
			else if (likelihood_type_ == "gamma") {
				return 0.;
			}
			else {
				Log::Fatal("SecondDerivLogCondMeanLikelihood: Likelihood of type '%s' is not supported.", likelihood_type_.c_str());
				return 0.;
			}
		}

		/*!
		* \brief Find the mode of the posterior of the latent random effects using Newton's method and calculate the approximative marginal log-likelihood..
		*		Calculations are done using a numerically stable variant based on B = (Id + Wsqrt * Z*Sigma*Zt * Wsqrt).
		*		In the notation of the paper: "Sigma = Z*Sigma*Z^T" and "Z = Id".
		*		This version is used for the Laplace approximation when dense matrices are used (e.g. GP models).
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param ZSigmaZt Covariance matrix of latent random effect (can be den_mat_t or sp_mat_t)
		* \param[out] approx_marginal_ll Approximate marginal log-likelihood evaluated at the mode
		*/
		template <typename T1>//T1 can be either den_mat_t or sp_mat_t
		void FindModePostRandEffCalcMLLStable(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data, const std::shared_ptr<T1> ZSigmaZt,
			double& approx_marginal_ll) {
			// Initialize variables
			if (!mode_initialized_) {
				InitializeModeAvec();
			}
			bool no_fixed_effects = (fixed_effects == nullptr);
			vec_t location_par;
			// Initialize objective function (LA approx. marginal likelihood) for use as convergence criterion
			if (no_fixed_effects) {
				approx_marginal_ll = LogLikelihood(y_data, y_data_int, mode_.data(), num_data);
			}
			else {
				location_par = vec_t(num_data);
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					location_par[i] = mode_[i] + fixed_effects[i];
				}
				approx_marginal_ll = LogLikelihood(y_data, y_data_int, location_par.data(), num_data);
			}
			double approx_marginal_ll_new;
			vec_t v_aux, v_aux2;//auxiliary variables
			sp_mat_t Wsqrt(num_data, num_data);//diagonal matrix with square root of negative second derivatives on the diagonal (sqrt of negative Hessian of log-likelihood)
			Wsqrt.setIdentity();
			T1 Id(num_data, num_data);
			Id.setIdentity();
			T1 Id_plus_Wsqrt_ZSigmaZt_Wsqrt;
			// Start finding mode 
			int it;
			for (it = 0; it < MAXIT_MODE_NEWTON_; ++it) {
				// Calculate first and second derivative of log-likelihood
				if (no_fixed_effects) {
					CalcFirstDerivLogLik(y_data, y_data_int, mode_.data(), num_data);
					CalcSecondDerivNegLogLik(y_data, y_data_int, mode_.data(), num_data);
				}
				else {
					CalcFirstDerivLogLik(y_data, y_data_int, location_par.data(), num_data);
					CalcSecondDerivNegLogLik(y_data, y_data_int, location_par.data(), num_data);
				}
				// Calculate Cholesky factor for calculation of approx. marginal log-likelihood (objective function) and for use in next iteration
				Wsqrt.diagonal().array() = second_deriv_neg_ll_.array().sqrt();
				Id_plus_Wsqrt_ZSigmaZt_Wsqrt = Id + Wsqrt * (*ZSigmaZt) * Wsqrt;
				chol_facts_Id_plus_Wsqrt_ZSigmaZt_Wsqrt_.compute(Id_plus_Wsqrt_ZSigmaZt_Wsqrt);
				// Update mode and a_vec_
				v_aux = second_deriv_neg_ll_.asDiagonal() * mode_ + first_deriv_ll_;
				v_aux2 = Wsqrt * (*ZSigmaZt) * v_aux;
				a_vec_ = v_aux - Wsqrt * (chol_facts_Id_plus_Wsqrt_ZSigmaZt_Wsqrt_.solve(v_aux2));
				mode_ = (*ZSigmaZt) * a_vec_;
				// Calculate new objective function
				if (no_fixed_effects) {
					approx_marginal_ll_new = -0.5 * (a_vec_.dot(mode_)) + LogLikelihood(y_data, y_data_int, mode_.data(), num_data);
				}
				else {
					// Update location parameter of log-likelihood for calculation of approx. marginal log-likelihood (objective function)
#pragma omp parallel for schedule(static)
					for (data_size_t i = 0; i < num_data; ++i) {
						location_par[i] = mode_[i] + fixed_effects[i];
					}
					approx_marginal_ll_new = -0.5 * (a_vec_.dot(mode_)) + LogLikelihood(y_data, y_data_int, location_par.data(), num_data);
				}
				if (std::abs(approx_marginal_ll_new - approx_marginal_ll) / std::abs(approx_marginal_ll) < DELTA_REL_CONV_) {
					approx_marginal_ll = approx_marginal_ll_new;
					break;
				}
				else {
					approx_marginal_ll = approx_marginal_ll_new;
				}
			}
			if (no_fixed_effects) {
				CalcFirstDerivLogLik(y_data, y_data_int, mode_.data(), num_data);//first derivative is not used here anymore but since it is reused in gradient calculation and in prediction, we calculate it once more
				CalcSecondDerivNegLogLik(y_data, y_data_int, mode_.data(), num_data);
			}
			else {
				CalcFirstDerivLogLik(y_data, y_data_int, location_par.data(), num_data);//first derivative is not used here anymore but since it is reused in gradient calculation and in prediction, we calculate it once more
				CalcSecondDerivNegLogLik(y_data, y_data_int, location_par.data(), num_data);
			}
			Wsqrt.diagonal().array() = second_deriv_neg_ll_.array().sqrt();
			Id_plus_Wsqrt_ZSigmaZt_Wsqrt = Id + Wsqrt * (*ZSigmaZt) * Wsqrt;
			chol_facts_Id_plus_Wsqrt_ZSigmaZt_Wsqrt_.compute(Id_plus_Wsqrt_ZSigmaZt_Wsqrt);
			approx_marginal_ll -= ((den_mat_t)chol_facts_Id_plus_Wsqrt_ZSigmaZt_Wsqrt_.matrixL()).diagonal().array().log().sum();
			mode_has_been_calculated_ = true;
			////Only for debugging -> delete this
			//Log::Info("Number of iterations: %d", it);
			//Log::Info("approx_marginal_ll: %g", approx_marginal_ll);
			//Log::Info("Mode");
			//for (int i = 0; i < 10; ++i) {
			//	Log::Info("mode_[%d]: %g", i, mode_[i]);
			//}
			//Log::Info("a");
			//for (int i = 0; i < 5; ++i) {
			//	Log::Info("a[%d]: %g", i, a_vec_[i]);
			//}
		}//end FindModePostRandEffCalcMLLStable

		/*!
		* \brief Find the mode of the posterior of the latent random effects using Newton's method and calculate the approximative marginal log-likelihood.
		*		Calculations are done by directly factorizing/inverting (Sigma^-1 + Zt*W*Z).
		*		NOTE: IT IS ASSUMED THAT SIGMA IS A DIAGONAL MATRIX
		*		This version is used for the Laplace approximation when there are only grouped random effects.
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param SigmaI Inverse covariance matrix of latent random effect. Currently, this needs to be a diagonal matrix
		* \param Zt Transpose Z^T of random effect design matrix that relates latent random effects to observations/likelihoods
		* \param[out] approx_marginal_ll Approximate marginal log-likelihood evaluated at the mode
		* \param only_one_random_effect true if there is only one random effect and ZtZ is diagonal
		*/
		void FindModePostRandEffCalcMLLGroupedRE(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data, const sp_mat_t& SigmaI, const sp_mat_t& Zt,
			double& approx_marginal_ll, bool only_one_random_effect = false) {
			// Initialize variables
			if (!mode_initialized_) {
				InitializeModeAvec();
			}
			sp_mat_t Z = Zt.transpose();
			vec_t location_par = Z * mode_;//location parameter = mode of random effects + fixed effects
			if (fixed_effects != nullptr) {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					location_par[i] += fixed_effects[i];
				}
			}
			// Initialize objective function (LA approx. marginal likelihood) for use as convergence criterion
			approx_marginal_ll = LogLikelihood(y_data, y_data_int, location_par.data(), num_data);
			double approx_marginal_ll_new;
			sp_mat_t SigmaI_plus_ZtWZ;
			vec_t rhs;
			// Start finding mode 
			int it;
			for (it = 0; it < MAXIT_MODE_NEWTON_; ++it) {
				// Calculate first and second derivative of log-likelihood
				CalcFirstDerivLogLik(y_data, y_data_int, location_par.data(), num_data);
				CalcSecondDerivNegLogLik(y_data, y_data_int, location_par.data(), num_data);
				// Calculate Cholesky factor for calculation of approx. marginal log-likelihood (objective function) and for use in next iteration
				// and update mode
				rhs = Zt * first_deriv_ll_ - SigmaI * mode_;//right hand side for updating mode
				if (only_one_random_effect) {
					sp_mat_t ZtWZ = Zt * second_deriv_neg_ll_.asDiagonal() * Z;
					diag_SigmaI_plus_ZtWZ_ = (SigmaI.diagonal().array() + ZtWZ.diagonal().array()).matrix();//Note: diagonal of (Sigma^-1 + Zt*W*Z) is saved
					mode_ += (rhs.array() / diag_SigmaI_plus_ZtWZ_.array()).matrix();
				}
				else {
					SigmaI_plus_ZtWZ = SigmaI + Zt * second_deriv_neg_ll_.asDiagonal() * Z;
					chol_facts_SigmaI_plus_ZtWZ_.compute(SigmaI_plus_ZtWZ);
					mode_ += chol_facts_SigmaI_plus_ZtWZ_.solve(rhs);
				}
				// Update location parameter of log-likelihood for calculation of approx. marginal log-likelihood (objective function)
				location_par = Z * mode_;
				if (fixed_effects != nullptr) {
#pragma omp parallel for schedule(static)
					for (data_size_t i = 0; i < num_data; ++i) {
						location_par[i] += fixed_effects[i];
					}
				}
				// Calculate new objective function
				approx_marginal_ll_new = -0.5 * (mode_.dot(SigmaI * mode_)) + LogLikelihood(y_data, y_data_int, location_par.data(), num_data);
				if (std::abs(approx_marginal_ll_new - approx_marginal_ll) / std::abs(approx_marginal_ll) < DELTA_REL_CONV_) {
					approx_marginal_ll = approx_marginal_ll_new;
					break;
				}
				else {
					approx_marginal_ll = approx_marginal_ll_new;
				}
			}//end mode finding algorithm
			CalcFirstDerivLogLik(y_data, y_data_int, location_par.data(), num_data);//first derivative is not used here anymore but since it is reused in gradient calculation and in prediction, we calculate it once more
			CalcSecondDerivNegLogLik(y_data, y_data_int, location_par.data(), num_data);
			if (only_one_random_effect) {
				sp_mat_t ZtWZ = Zt * second_deriv_neg_ll_.asDiagonal() * Z;
				diag_SigmaI_plus_ZtWZ_ = (SigmaI.diagonal().array() + ZtWZ.diagonal().array()).matrix();
				approx_marginal_ll -= 0.5 * diag_SigmaI_plus_ZtWZ_.array().log().sum() - 0.5 * SigmaI.diagonal().array().log().sum();
			}
			else {
				SigmaI_plus_ZtWZ = SigmaI + Zt * second_deriv_neg_ll_.asDiagonal() * Z;
				chol_facts_SigmaI_plus_ZtWZ_.compute(SigmaI_plus_ZtWZ);
				approx_marginal_ll += -((den_mat_t)chol_facts_SigmaI_plus_ZtWZ_.matrixL()).diagonal().array().log().sum() + 0.5 * SigmaI.diagonal().array().log().sum();
			}
			mode_has_been_calculated_ = true;
			////Only for debugging -> delete this
			//double approx_marginal_ll_1 = -0.5 * (mode_.dot(SigmaI * mode_)); 
			//double approx_marginal_ll_2 = LogLikelihood(y_data, y_data_int, location_par.data(), num_data);
			//double approx_marginal_ll_3 = 0.5 * diag_SigmaI_plus_ZtWZ_.array().log().sum() - 0.5 * SigmaI.diagonal().array().log().sum();
			//Log::Info("");
			//Log::Info("Number of iterations: %d", it);
			//Log::Info("approx_marginal_ll_1: %g", approx_marginal_ll_1);
			//Log::Info("approx_marginal_ll_2: %g", approx_marginal_ll_2);
			//Log::Info("approx_marginal_ll_3: %g", approx_marginal_ll_3);
			//Log::Info("Mode");
			//for (int i = 0; i < 10; ++i) {
			//	Log::Info("mode_[%d]: %g", i, mode_[i]);
			//}
			//std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}//end FindModePostRandEffCalcMLLGroupedRE

		/*!
		* \brief Find the mode of the posterior of the latent random effects using Newton's method and calculate the approximative marginal log-likelihood.
		*		Calculations are done by inverting (Sigma^-1 + W) where it is assumed that an approximate Cholesky factor of Sigma^-1 has
		*		previously been calculated using a Vecchia approximation.
		*		This version is used for the Laplace approximation when there are only GP radnom effects and the Vecchia approximation is used.
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param B Matrix B in Vecchia approximation Sigma^-1 = B^T D^-1 B ("=" Cholesky factor)
		* \param D_inv Diagonal matrix D^-1 in Vecchia approximation Sigma^-1 = B^T D^-1 B
		* \param[out] approx_marginal_ll Approximate marginal log-likelihood evaluated at the mode
		*/
		void FindModePostRandEffCalcMLLVecchia(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data, const sp_mat_t& B, const sp_mat_t& D_inv,
			double& approx_marginal_ll) {
			// Initialize variables
			if (!mode_initialized_) {
				InitializeModeAvec();
			}
			bool no_fixed_effects = (fixed_effects == nullptr);
			sp_mat_t SigmaI = B.transpose() * D_inv * B;
			vec_t location_par;//location parameter = mode of random effects + fixed effects
			// Initialize objective function (LA approx. marginal likelihood) for use as convergence criterion
			if (no_fixed_effects) {
				approx_marginal_ll = LogLikelihood(y_data, y_data_int, mode_.data(), num_data);
			}
			else {
				location_par = vec_t(num_data);
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					location_par[i] = mode_[i] + fixed_effects[i];
				}
				approx_marginal_ll = LogLikelihood(y_data, y_data_int, location_par.data(), num_data);
			}
			double approx_marginal_ll_new;
			sp_mat_t SigmaI_plus_W;
			vec_t rhs, B_mode;
			// Start finding mode 
			int it;
			for (it = 0; it < MAXIT_MODE_NEWTON_; ++it) {
				// Calculate first and second derivative of log-likelihood
				if (no_fixed_effects) {
					CalcFirstDerivLogLik(y_data, y_data_int, mode_.data(), num_data);
					CalcSecondDerivNegLogLik(y_data, y_data_int, mode_.data(), num_data);
				}
				else {
					CalcFirstDerivLogLik(y_data, y_data_int, location_par.data(), num_data);
					CalcSecondDerivNegLogLik(y_data, y_data_int, location_par.data(), num_data);
				}
				// Calculate Cholesky factor for calculation of approx. marginal log-likelihood (objective function) and for use in next iteration
				// and update mode
				rhs = second_deriv_neg_ll_.asDiagonal() * mode_ + first_deriv_ll_;//right hand side for updating mode
				SigmaI_plus_W = SigmaI;
				SigmaI_plus_W.diagonal().array() += second_deriv_neg_ll_.array();
				//Log::Info("Number non zeros = %d", (int)SigmaI_plus_W.nonZeros());//only for debugging, can be deleted
				chol_facts_SigmaI_plus_ZtWZ_.compute(SigmaI_plus_W);//This is usually the bottleneck
				mode_ = chol_facts_SigmaI_plus_ZtWZ_.solve(rhs);
				// Calculate new objective function
				B_mode = B * mode_;
				if (no_fixed_effects) {
					approx_marginal_ll_new = -0.5 * (B_mode.dot(D_inv * B_mode)) + LogLikelihood(y_data, y_data_int, mode_.data(), num_data);
				}
				else {
					// Update location parameter of log-likelihood for calculation of approx. marginal log-likelihood (objective function)
#pragma omp parallel for schedule(static)
					for (data_size_t i = 0; i < num_data; ++i) {
						location_par[i] = mode_[i] + fixed_effects[i];
					}
					approx_marginal_ll_new = -0.5 * (B_mode.dot(D_inv * B_mode)) + LogLikelihood(y_data, y_data_int, location_par.data(), num_data);
				}
				if (std::abs(approx_marginal_ll_new - approx_marginal_ll) / std::abs(approx_marginal_ll) < DELTA_REL_CONV_) {
					approx_marginal_ll = approx_marginal_ll_new;
					break;
				}
				else {
					approx_marginal_ll = approx_marginal_ll_new;
				}
			}
			if (no_fixed_effects) {
				CalcFirstDerivLogLik(y_data, y_data_int, mode_.data(), num_data);//first derivative is not used here anymore but since it is reused in gradient calculation and in prediction, we calculate it once more
				CalcSecondDerivNegLogLik(y_data, y_data_int, mode_.data(), num_data);
			}
			else {
				CalcFirstDerivLogLik(y_data, y_data_int, location_par.data(), num_data);//first derivative is not used here anymore but since it is reused in gradient calculation and in prediction, we calculate it once more
				CalcSecondDerivNegLogLik(y_data, y_data_int, location_par.data(), num_data);
			}
			SigmaI_plus_W = SigmaI;
			SigmaI_plus_W.diagonal().array() += second_deriv_neg_ll_.array();
			chol_facts_SigmaI_plus_ZtWZ_.compute(SigmaI_plus_W);
			approx_marginal_ll += -((den_mat_t)chol_facts_SigmaI_plus_ZtWZ_.matrixL()).diagonal().array().log().sum() + 0.5 * D_inv.diagonal().array().log().sum();
			mode_has_been_calculated_ = true;
			////Only for debugging -> delete this
			//Log::Info("Number of iterations: %d", it);
			//Log::Info("approx_marginal_ll: %g", approx_marginal_ll);
			//Log::Info("Mode");
			//for (int i = 0; i < 10; ++i) {
			//	Log::Info("mode_[%d]: %g", i, mode_[i]);
			//}
			//std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}//end FindModePostRandEffCalcMLLVecchia


		/*!
		* \brief Calculate the gradient of the negative Laplace approximated marginal log-likelihood wrt covariance parameters, fixed effects, or linear regression coefficients
		*		Calculations are done using a numerically stable variant based on B = (Id + Wsqrt * ZSigmaZt * Wsqrt).
		*		In the notation of the paper: "Sigma = Z*Sigma*Z^T" and "Z = Id".
		*		This version is used for the Laplace approximation when dense matrices are used (e.g. GP models).
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param ZSigmaZt Covariance matrix of latent random effect (can be den_mat_t or sp_mat_t)
		* \param re_comps_cluster_i Vector with different random effects components. We pass the component pointers to save memory in order to avoid passing a large collection of gardient covariance matrices in memory//TODO: better way than passing this? (relying on all gradients in a vector can lead to large memory consumption)
		* \param calc_cov_grad If true, the gradient wrt the covariance parameters are calculated
		* \param calc_F_grad If true, the gradient wrt the fixed effects mean function F are calculated
		* \param[out] cov_grad Gradient of approximate marginal log-likelihood wrt covariance parameters (needs to be preallocated of size num_cov_par)
		* \param[out] fixed_effect_grad Gradient of approximate marginal log-likelihood wrt fixed effects F (note: this is passed as a Eigen vector in order to avoid the need for copying)
		* \param calc_mode If true, the mode of the random effects posterior is calculated otherwise the values in mode and a_vec_ are used (default=false)
		*/
		template <typename T1>//T1 can be either den_mat_t or sp_mat_t
		void CalcGradNegMargLikelihoodLAApproxStable(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data,
			const std::shared_ptr<T1> ZSigmaZt, const std::vector<std::shared_ptr<RECompBase<T1>>>& re_comps_cluster_i,
			bool calc_cov_grad, bool calc_F_grad,
			double* cov_grad, vec_t& fixed_effect_grad, bool calc_mode = false) {
			if (calc_mode) {// Calculate mode and Cholesky factor of B = (Id + Wsqrt * ZSigmaZt * Wsqrt) at mode
				double mll;//approximate marginal likelihood. This is a by-product that is not used here.
				FindModePostRandEffCalcMLLStable<T1>(y_data, y_data_int, fixed_effects, num_data, ZSigmaZt, mll);
			}
			else {
				CHECK(mode_has_been_calculated_);
			}
			// Initialize variables
			bool no_fixed_effects = (fixed_effects == nullptr);
			vec_t location_par;//location parameter = mode of random effects + fixed effects
			sp_mat_t Wsqrt(num_data, num_data);//diagonal matrix with square root of negative second derivatives on the diagonal (sqrt of negative Hessian of log-likelihood)
			Wsqrt.setIdentity();
			vec_t third_deriv(num_data);//vector of third derivatives of log-likelihood
			if (no_fixed_effects) {
				CalcThirdDerivLogLik(y_data, y_data_int, mode_.data(), num_data, third_deriv.data());
			}
			else {
				location_par = vec_t(num_data);
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					location_par[i] = mode_[i] + fixed_effects[i];
				}
				CalcThirdDerivLogLik(y_data, y_data_int, location_par.data(), num_data, third_deriv.data());
			}
			Wsqrt.diagonal().array() = second_deriv_neg_ll_.array().sqrt();
			T1 L = chol_facts_Id_plus_Wsqrt_ZSigmaZt_Wsqrt_.matrixL();
			T1 L_inv_Wsqrt, WI_plus_Sigma_inv, C;
			CalcLInvH(L, Wsqrt, L_inv_Wsqrt, true);//L_inv_Wsqrt = L\Wsqrt		
			C = L_inv_Wsqrt * (*ZSigmaZt);
			// calculate gradient wrt covariance parameters
			if (calc_cov_grad) {
				//CalcLInvH(L, L_inv_Wsqrt, WI_plus_Sigma_inv, false);//WI_plus_Sigma_inv = Wsqrt * L^T\(L\Wsqrt) = (W^-1 + Sigma)^-1
				//WI_plus_Sigma_inv = Wsqrt * WI_plus_Sigma_inv;
				WI_plus_Sigma_inv = L_inv_Wsqrt.transpose() * L_inv_Wsqrt;//WI_plus_Sigma_inv = Wsqrt * L^T\(L\Wsqrt) = (W^-1 + Sigma)^-1
				// calculate gradient of approx. marginal log-likelihood wrt the mode
				// note: use (i) (Sigma^-1 + W)^-1 = Sigma - Sigma*(W^-1 + Sigma)^-1*Sigma = ZSigmaZt - C^T*C and (ii) "Z=Id"
				vec_t d_mll_d_mode = (-0.5 * ((*ZSigmaZt).diagonal() - ((T1)(C.transpose() * C)).diagonal()).array() * third_deriv.array()).matrix();
				vec_t d_mode_d_par;//derivative of mode wrt to a covariance parameter
				vec_t v_aux;//auxiliary variable for caclulating d_mode_d_par
				int par_count = 0;
				double explicit_derivative;
				for (int j = 0; j < (int)re_comps_cluster_i.size(); ++j) {
					for (int ipar = 0; ipar < re_comps_cluster_i[j]->NumCovPar(); ++ipar) {
						std::shared_ptr<T1> SigmaDeriv = re_comps_cluster_i[j]->GetZSigmaZtGrad(ipar, true, 1.);
						// calculate explicit derivative of approx. mariginal log-likelihood
						explicit_derivative = -0.5 * (double)(a_vec_.transpose() * (*SigmaDeriv) * a_vec_) + 0.5 * (WI_plus_Sigma_inv.cwiseProduct(*SigmaDeriv)).sum();
						// calculate implicit derivative (through mode) of approx. mariginal log-likelihood
						v_aux = (*SigmaDeriv) * first_deriv_ll_;
						d_mode_d_par = (v_aux.array() - ((*ZSigmaZt) * WI_plus_Sigma_inv * v_aux).array()).matrix();
						cov_grad[par_count] = explicit_derivative + d_mll_d_mode.dot(d_mode_d_par);
						par_count++;
					}
				}
				////Only for debugging -> delete this
				//Log::Info("explicit_derivative: %g", explicit_derivative);
				//for (int i = 0; i < 5; ++i) {
				//	Log::Info("d_mll_d_mode[%d]: %g", i, d_mll_d_mode[i]);
				//}
				//for (int i = 0; i < 5; ++i) {
				//	Log::Info("d_mode_d_par[%d]: %g", i, d_mode_d_par[i]);
				//}
				//Log::Info("cov_grad");
				//for (int i = 0; i < par_count; ++i) {
				//	Log::Info("cov_grad[%d]: %g", i, cov_grad[i]);
				//}
			}//end calc_cov_grad
			// calculate gradient wrt fixed effects
			if (calc_F_grad) {
				T1 ZSigmaZtI_plus_W_inv = (*ZSigmaZt) - (T1)(C.transpose() * C);// = (ZSigmaZt^-1 + W) ^ -1
				// calculate gradient of approx. marginal likeligood wrt the mode
				vec_t d_mll_d_mode = (-0.5 * ZSigmaZtI_plus_W_inv.diagonal().array() * third_deriv.array()).matrix();//Note: d_mll_d_mode = d_detmll_d_F
				//T1 ZSigmaZtI_plus_W_inv_W = ZSigmaZtI_plus_W_inv * second_deriv_neg_ll_.asDiagonal();//DELETE
				//fixed_effect_grad = -first_deriv_ll_ + d_mll_d_mode - d_mll_d_mode.transpose() * ZSigmaZtI_plus_W_inv_W;//DELETE
				vec_t d_mll_d_modeT_SigmaI_plus_ZtWZ_inv_Zt_W = d_mll_d_mode.transpose() * ZSigmaZtI_plus_W_inv * second_deriv_neg_ll_.asDiagonal();
				fixed_effect_grad = -first_deriv_ll_ + d_mll_d_mode - d_mll_d_modeT_SigmaI_plus_ZtWZ_inv_Zt_W;
			}//end calc_F_grad
		}//end CalcGradNegMargLikelihoodLAApproxStable


		/*!
		* \brief Calculate the gradient of the negative Laplace approximated marginal log-likelihood wrt covariance parameters, fixed effects, or linear regression coefficients
		*		Calculations are done by directly factorizing/inverting (Sigma^-1 + Zt*W*Z).
		*		In the notation of the paper: "Sigma = Z*Sigma*Z^T" and "Z = Id".
		*		NOTE: IT IS ASSUMED THAT SIGMA IS A DIAGONAL MATRIX
		*		This version is used for the Laplace approximation when there are only grouped random effects.
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param SigmaI Inverse covariance matrix of latent random effect. Currently, this needs to be a diagonal matrix
		* \param Zt Transpose Z^T of random effect design matrix that relates latent random effects to observations/likelihoods
		* \param re_comps_cluster_i Vector with different random effects components. We pass the component pointers to save memory in order to avoid passing a large collection of gardient covariance matrices in memory//TODO: better way than passing this? (relying on all gradients in a vector can lead to large memory consumption)
		* \param calc_cov_grad If true, the gradient wrt the covariance parameters are calculated
		* \param calc_F_grad If true, the gradient wrt the fixed effects mean function F are calculated
		* \param[out] cov_grad Gradient wrt covariance parameters (needs to be preallocated of size num_cov_par)
		* \param[out] fixed_effect_grad Gradient wrt fixed effects F (note: this is passed as a Eigen vector in order to avoid the need for copying)
		* \param calc_mode If true, the mode of the random effects posterior is calculated otherwise the values in mode and a_vec_ are used (default=false)
		* \param only_one_random_effect true if there is only one random effect and ZtZ is diagonal
		*/
		template <typename T1>//T1 can be either den_mat_t or sp_mat_t
		void CalcGradNegMargLikelihoodLAApproxGroupedRE(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data,
			const sp_mat_t& SigmaI, const sp_mat_t& Zt, const std::vector<std::shared_ptr<RECompBase<T1>>>& re_comps_cluster_i,
			std::vector<data_size_t> cum_num_rand_eff_cluster_i, bool calc_cov_grad, bool calc_F_grad,
			double* cov_grad, vec_t& fixed_effect_grad, bool calc_mode = false, bool only_one_random_effect = false) {
			int num_REs = (int)SigmaI.cols();//number of random effect realizations
			int num_comps = (int)re_comps_cluster_i.size();//number of different random effect components
			if (calc_mode) {// Calculate mode and Cholesky factor of Sigma^-1 + W at mode
				double mll;//approximate marginal likelihood. This is a by-product that is not used here.
				FindModePostRandEffCalcMLLGroupedRE(y_data, y_data_int, fixed_effects, num_data, SigmaI, Zt,
					mll, only_one_random_effect);
			}
			else {
				CHECK(mode_has_been_calculated_);
			}
			// Initialize variables
			sp_mat_t Z = Zt.transpose();
			vec_t location_par = Z * mode_;//location parameter = mode of random effects + fixed effects
			if (fixed_effects != nullptr) {
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					location_par[i] += fixed_effects[i];
				}
			}
			vec_t third_deriv(num_data);//vector of third derivatives of log-likelihood
			CalcThirdDerivLogLik(y_data, y_data_int, location_par.data(), num_data, third_deriv.data());
			// Calculate (Sigma^-1 + Zt*W*Z)^-1
			sp_mat_t SigmaI_plus_ZtWZ_inv;
			if (only_one_random_effect) {
				SigmaI_plus_ZtWZ_inv = diag_SigmaI_plus_ZtWZ_.array().inverse().matrix().asDiagonal();
			}
			else {
				sp_mat_t Id(num_REs, num_REs);
				Id.setIdentity();
				SigmaI_plus_ZtWZ_inv = chol_facts_SigmaI_plus_ZtWZ_.solve(Id);
			}
			// calculate gradient of approx. marginal likeligood wrt the mode
			vec_t d_mll_d_mode;
			if (only_one_random_effect) {
				d_mll_d_mode = vec_t::Zero(num_REs);
				std::shared_ptr<RECompGroup<T1>> re = std::dynamic_pointer_cast<RECompGroup<T1>>(re_comps_cluster_i[0]);
				for (int i = 0; i < num_data; ++i) {//TODO: run this in parallel
					int re_nb = (*re->map_group_label_index_)[(*re->group_data_)[i]];
					d_mll_d_mode[re_nb] += third_deriv[i];
				}
#pragma omp parallel for schedule(static)
				for (int i = 0; i < num_REs; ++i) {
					d_mll_d_mode[i] *= -0.5 * SigmaI_plus_ZtWZ_inv.coeffRef(i, i);
				}
			}//end only_one_random_effect
			else {//not only_one_random_effect
				//Note: the calculation of d_mll_d_mode is the bottleneck of this function (corresponding lines below are indicated with * and, in particular, **)
				d_mll_d_mode = vec_t(num_REs);
				sp_mat_t Zt_third_deriv = Zt * third_deriv.asDiagonal();//every column of Z multiplied elementwise by third_deriv
#pragma omp parallel for schedule(static)
				for (int i = 0; i < num_REs; ++i) {
					vec_t diag_d_W_d_mode_i = Zt_third_deriv.row(i);//*can be slow
					//calculate Z^T * diag(diag_d_W_d_mode_i) * Z = Z^T * diag(Z.col(i) * third_deriv) * Z
					sp_mat_t Zt_d_W_d_mode_i_Z = (Zt * diag_d_W_d_mode_i.asDiagonal() * Z).pruned();//**can be very slow. Note that this is also slow when the middle diagonal matrix is a pruned sparse matrix
					////Variant 2: slower
					//sp_mat_t Zt_third_deriv_diag = sp_mat_t(((vec_t)Zt_third_deriv.row(i)).asDiagonal());
					//sp_mat_t Zt_d_W_d_mode_i_Z = Zt * Zt_third_deriv_diag * Z;//= Z^T * diag(diag_d_W_d_mode_i) * Z = Z^T * diag(Z.col(i) * third_deriv) * Z
					////Variant 3: slower
					//vec_t Z_i = Z.col(i);// column number i of Z
					//vec_t diag_d_W_d_mode_i = (Z_i.array() * third_deriv.array()).matrix();//diagonal of derivative of matrix W wrt random effect number i
					//sp_mat_t Zt_d_W_d_mode_i_Z = Zt * diag_d_W_d_mode_i.asDiagonal() * Z;//= Z^T * diag(diag_d_W_d_mode_i) * Z
					d_mll_d_mode[i] = -0.5 * (Zt_d_W_d_mode_i_Z.cwiseProduct(SigmaI_plus_ZtWZ_inv)).sum();
				}
			}//end not only_one_random_effect
			// calculate gradient wrt covariance parameters
			if (calc_cov_grad) {
				sp_mat_t ZtWZ = Zt * second_deriv_neg_ll_.asDiagonal() * Z;
				vec_t d_mode_d_par;//derivative of mode wrt to a covariance parameter
				vec_t v_aux;//auxiliary variable for caclulating d_mode_d_par
				vec_t SigmaI_mode = SigmaI * mode_;
				double explicit_derivative;
				if (only_one_random_effect) {
					explicit_derivative = 0.;
#pragma omp parallel for schedule(static) reduction(+:explicit_derivative)
					for (int i = 0; i < num_REs; ++i) {
						explicit_derivative += SigmaI_mode[i] * mode_[i];
					}
					explicit_derivative *= -0.5;
					explicit_derivative += 0.5 * (SigmaI_plus_ZtWZ_inv.cwiseProduct(ZtWZ)).sum();
					// calculate implicit derivative (through mode) of approx. mariginal log-likelihood
					d_mode_d_par = SigmaI_plus_ZtWZ_inv * Zt * first_deriv_ll_;
					cov_grad[0] = explicit_derivative + d_mll_d_mode.dot(d_mode_d_par);
				}//end only_one_random_effect
				else {//not only_one_random_effect
					sp_mat_t I_j(num_REs, num_REs);//Diagonal matrix with 1 on the diagonal for all random effects of component j and 0's otherwise
					sp_mat_t I_j_ZtWZ;
					for (int j = 0; j < num_comps; ++j) {
						// calculate explicit derivative of approx. mariginal log-likelihood
						std::vector<Triplet_t> triplets;//for constructing I_j
						triplets.reserve(cum_num_rand_eff_cluster_i[j + 1] - cum_num_rand_eff_cluster_i[j]);
						explicit_derivative = 0.;
						for (int i = cum_num_rand_eff_cluster_i[j]; i < cum_num_rand_eff_cluster_i[j + 1]; ++i) {
							triplets.emplace_back(i, i, 1.);
							explicit_derivative += SigmaI_mode[i] * mode_[i];
						}
						// Altervative version using parallelization (not faster)
						//#pragma omp parallel
						//					{
						//						std::vector<Triplet_t> triplets_private;
						//						//triplets_private.reserve(cum_num_rand_eff_cluster_i[num_comps]);
						//#pragma omp for nowait reduction(+:explicit_derivative)
						//						for (int i = cum_num_rand_eff_cluster_i[j]; i < cum_num_rand_eff_cluster_i[j + 1]; ++i) {
						//							triplets_private.emplace_back(i, i, 1.);
						//							explicit_derivative += SigmaI_mode[i] * mode_[i];
						//						}
						//#pragma omp critical
						//						triplets.insert(triplets.end(), triplets_private.begin(), triplets_private.end());
						//					}
						//#pragma omp parallel for schedule(static) reduction(+:explicit_derivative)
						//					for (int i = cum_num_rand_eff_cluster_i[j]; i < cum_num_rand_eff_cluster_i[j + 1]; ++i) {
						//						explicit_derivative += SigmaI_mode[i] * mode_[i];
						//					}
						explicit_derivative *= -0.5;
						I_j.setFromTriplets(triplets.begin(), triplets.end());
						I_j_ZtWZ = I_j * ZtWZ;
						explicit_derivative += 0.5 * (SigmaI_plus_ZtWZ_inv.cwiseProduct(I_j_ZtWZ)).sum();
						// calculate implicit derivative (through mode) of approx. mariginal log-likelihood
						d_mode_d_par = SigmaI_plus_ZtWZ_inv * I_j * Zt * first_deriv_ll_;
						cov_grad[j] = explicit_derivative + d_mll_d_mode.dot(d_mode_d_par);
					}
				}//end not only_one_random_effect
				//Only for debugging -> delete this
				//Log::Info("explicit_derivative: %g", explicit_derivative);
				//for (int i = 0; i < 5; ++i) {
				//	Log::Info("d_mll_d_mode[%d]: %g", i, d_mll_d_mode[i]);
				//}
				//for (int i = 0; i < 5; ++i) {
				//	Log::Info("d_mode_d_par[%d]: %g", i, d_mode_d_par[i]);
				//}
				//Log::Info("cov_grad");
				//for (int i = 0; i < num_comps; ++i) {
				//	Log::Info("cov_grad[%d]: %g", i, cov_grad[i]);
				//}
			}//end calc_cov_grad
			// calculate gradient wrt fixed effects
			if (calc_F_grad) {
				vec_t d_detmll_d_F(num_data);
#pragma omp parallel for schedule(static)
				for (int i = 0; i < num_data; ++i) {
					//vec_t zi = Z.row(i);
					//den_mat_t zi_zit = zi * zi.transpose();
					sp_mat_t zi_zit = Zt.col(i) * Z.row(i);//=Z.row(i) * (Z.row(i)).transpose()
					d_detmll_d_F[i] = -0.5 * third_deriv[i] * (SigmaI_plus_ZtWZ_inv.cwiseProduct(zi_zit)).sum();
				}
				//sp_mat_t SigmaI_plus_ZtWZ_inv_Zt_W = SigmaI_plus_ZtWZ_inv * Zt * second_deriv_neg_ll_.asDiagonal();//DELETE
				vec_t d_mll_d_modeT_SigmaI_plus_ZtWZ_inv_Zt_W = d_mll_d_mode.transpose() * SigmaI_plus_ZtWZ_inv * Zt * second_deriv_neg_ll_.asDiagonal();
				fixed_effect_grad = -first_deriv_ll_ + d_detmll_d_F - d_mll_d_modeT_SigmaI_plus_ZtWZ_inv_Zt_W;
			}//end calc_F_grad
		}//end CalcGradNegMargLikelihoodLAApproxGroupedRE


		/*!
		* \brief Calculate the gradient of the negative Laplace approximated marginal log-likelihood wrt covariance parameters, fixed effects, or linear regression coefficients
		*		Calculations are done by inverting (Sigma^-1 + W) where it is assumed that an approximate Cholesky factor of Sigma^-1 has
		*		previously been calculated using a Vecchia approximation.
		*		This version is used for the Laplace approximation when there are only GP radnom effects and the Vecchia approximation is used.
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param B Matrix B in Vecchia approximation Sigma^-1 = B^T D^-1 B ("=" Cholesky factor)
		* \param D_inv Diagonal matrix D^-1 in Vecchia approximation Sigma^-1 = B^T D^-1 B
		* \param B_grad Derivatives of matrices B ( = derivative of matrix -A) for Vecchia approximation
		* \param D_grad Derivatives of matrices D for Vecchia approximation
		* \param calc_cov_grad If true, the gradient wrt the covariance parameters are calculated
		* \param calc_F_grad If true, the gradient wrt the fixed effects mean function F are calculated
		* \param[out] cov_grad Gradient of approximate marginal log-likelihood wrt covariance parameters (needs to be preallocated of size num_cov_par)
		* \param[out] fixed_effect_grad Gradient of approximate marginal log-likelihood wrt fixed effects F (note: this is passed as a Eigen vector in order to avoid the need for copying)
		* \param calc_mode If true, the mode of the random effects posterior is calculated otherwise the values in mode and a_vec_ are used (default=false)
		*/
		void CalcGradNegMargLikelihoodLAApproxVecchia(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data,
			const sp_mat_t& B, const sp_mat_t& D_inv, const std::vector<sp_mat_t>& B_grad, const std::vector<sp_mat_t>& D_grad,
			bool calc_cov_grad, bool calc_F_grad, double* cov_grad, vec_t& fixed_effect_grad, bool calc_mode = false) {
			if (calc_mode) {// Calculate mode and Cholesky factor of Sigma^-1 + W at mode
				double mll;//approximate marginal likelihood. This is a by-product that is not used here.
				FindModePostRandEffCalcMLLVecchia(y_data, y_data_int, fixed_effects, num_data, B, D_inv, mll);
			}
			else {
				CHECK(mode_has_been_calculated_);
			}
			// Initialize variables
			bool no_fixed_effects = (fixed_effects == nullptr);
			vec_t location_par;//location parameter = mode of random effects + fixed effects
			vec_t third_deriv(num_data);//vector of third derivatives of log-likelihood
			if (no_fixed_effects) {
				CalcThirdDerivLogLik(y_data, y_data_int, mode_.data(), num_data, third_deriv.data());
			}
			else {
				location_par = vec_t(num_data);
#pragma omp parallel for schedule(static)
				for (data_size_t i = 0; i < num_data; ++i) {
					location_par[i] = mode_[i] + fixed_effects[i];
				}
				CalcThirdDerivLogLik(y_data, y_data_int, location_par.data(), num_data, third_deriv.data());
			}
			// Calculate (Sigma^-1 + W)^-1
			sp_mat_t Id(num_data, num_data);
			Id.setIdentity();
			sp_mat_t SigmaI_plus_W_inv = chol_facts_SigmaI_plus_ZtWZ_.solve(Id);
			// calculate gradient of approx. marginal likeligood wrt the mode
			vec_t d_mll_d_mode = -0.5 * (SigmaI_plus_W_inv.diagonal().array() * third_deriv.array()).matrix();
			// calculate gradient wrt covariance parameters
			if (calc_cov_grad) {
				vec_t d_mode_d_par;//derivative of mode wrt to a covariance parameter
				double explicit_derivative;
				int num_par = (int)B_grad.size();
				sp_mat_t SigmaI_deriv;
				sp_mat_t BgradT_Dinv_B;
				sp_mat_t Bt_Dinv_Bgrad;
				for (int j = 0; j < num_par; ++j) {
					SigmaI_deriv = B_grad[j].transpose() * D_inv * B;
					Bt_Dinv_Bgrad = SigmaI_deriv.transpose();
					SigmaI_deriv += Bt_Dinv_Bgrad - B.transpose() * D_inv * D_grad[j] * D_inv * B;
					d_mode_d_par = -SigmaI_plus_W_inv * SigmaI_deriv * mode_;
					explicit_derivative = 0.5 * mode_.dot(SigmaI_deriv * mode_) +
						0.5 * ((D_inv.diagonal().array() * D_grad[j].diagonal().array()).sum() + (SigmaI_deriv.cwiseProduct(SigmaI_plus_W_inv)).sum());
					// Alternative version (not faster)
					//vec_t u = D_inv * B * mode_;
					//vec_t uk = B_grad[j] * mode_;
					//explicit_derivative = uk.dot(u) - 0.5 * u.dot(D_grad[j] * u) +
					//	0.5 * ((D_inv.diagonal().array() * D_grad[j].diagonal().array()).sum() + (SigmaI_deriv.cwiseProduct(SigmaI_plus_W_inv)).sum());
					cov_grad[j] = explicit_derivative + d_mll_d_mode.dot(d_mode_d_par);
				}
				////Only for debugging -> delete this				
				//Log::Info("explicit_derivative: %g", explicit_derivative);
				//for (int i = 0; i < 5; ++i) {
				//	Log::Info("d_mll_d_mode[%d]: %g", i, d_mll_d_mode[i]);
				//}
				//for (int i = 0; i < 5; ++i) {
				//	Log::Info("d_mode_d_par[%d]: %g", i, d_mode_d_par[i]);
				//}
				////Only for debugging -> delete this
				//Log::Info("cov_grad");
				//for (int i = 0; i < num_par; ++i) {
				//	Log::Info("cov_grad[%d]: %g", i, cov_grad[i]);
				//}
			}//end calc_cov_grad
			// calculate gradient wrt fixed effects
			if (calc_F_grad) {
				vec_t impl_deriv = -d_mll_d_mode.transpose() * SigmaI_plus_W_inv * second_deriv_neg_ll_.asDiagonal();
				fixed_effect_grad = -first_deriv_ll_ + d_mll_d_mode + impl_deriv;
			}//end calc_F_grad



		}//end CalcGradNegMargLikelihoodLAApproxVecchia


		/*!
		* \brief Make predictions for the (latent) random effects when using the Laplace approximation.
		*		Calculations are done using a numerically stable variant based on B = (Id + Wsqrt * ZSigmaZt * Wsqrt).
		*		This version is used for the Laplace approximation when there are GP random effects.
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param ZSigmaZt Covariance matrix of latent random effect (can be den_mat_t or sp_mat_t)
		* \param Cross_Cov Cross covariance matrix between predicted and obsreved random effects ("=Cov(y_p,y)")
		* \param pred_mean[out] Predicted mean
		* \param pred_cov[out] Predicted covariance matrix
		* \param pred_var[out] Predicted variances
		* \param calc_pred_cov If true, predictive covariance matrix is also calculated
		* \param calc_pred_var If true, predictive variances are also calculated
		* \param calc_mode If true, the mode of the random effects posterior is calculated otherwise the values in mode and a_vec_ are used (default=false)
		*/
		template <typename T1>//T1 can be either den_mat_t or sp_mat_t
		void PredictLAApproxStable(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data, const std::shared_ptr<T1> ZSigmaZt, T1& Cross_Cov,
			vec_t& pred_mean, T1& pred_cov, vec_t& pred_var, bool calc_pred_cov = false, bool calc_pred_var = false, bool calc_mode = false) {
			if (calc_mode) {// Calculate mode and Cholesky factor of B = (Id + Wsqrt * ZSigmaZt * Wsqrt) at mode
				double mll;//approximate marginal likelihood. This is a by-product that is not used here.
				FindModePostRandEffCalcMLLStable<T1>(y_data, y_data_int, fixed_effects, num_data, ZSigmaZt, mll);
			}
			else {
				CHECK(mode_has_been_calculated_);
			}
			pred_mean = Cross_Cov * first_deriv_ll_;
			if (calc_pred_cov || calc_pred_var) {
				sp_mat_t Wsqrt(num_data, num_data);//diagonal matrix with square root of negative second derivatives on the diagonal (sqrt of negative Hessian of log-likelihood)
				Wsqrt.setIdentity();
				Wsqrt.diagonal().array() = second_deriv_neg_ll_.array().sqrt();
				T1 L = chol_facts_Id_plus_Wsqrt_ZSigmaZt_Wsqrt_.matrixL();
				T1 Maux, Maux2;
				Maux = Wsqrt * Cross_Cov.transpose();
				CalcLInvH(L, Maux, Maux2, true);//Maux2 = L\(Wsqrt * Cross_Cov^T)
				if (calc_pred_cov) {
					pred_cov -= Maux2.transpose() * Maux2;
				}
				if (calc_pred_var) {
					Maux2 = Maux2.cwiseProduct(Maux2);
#pragma omp parallel for schedule(static)
					for (int i = 0; i < (int)pred_mean.size(); ++i) {
						pred_var[i] -= Maux2.col(i).sum();
					}
				}
			}
		}//end PredictLAApproxStable

		/*!
		* \brief Make predictions for the (latent) random effects when using the Laplace approximation.
		*		Calculations are done by directly factorizing/inverting (Sigma^-1 + Zt*W*Z).
		*		In the notation of the paper: "Sigma = Z*Sigma*Z^T" and "Z = Id".
		*		NOTE: IT IS ASSUMED THAT SIGMA IS A DIAGONAL MATRIX
		*		This version is used for the Laplace approximation when there are only grouped random effects.
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param SigmaI Inverse covariance matrix of latent random effect. Currently, this needs to be a diagonal matrix
		* \param Zt Transpose Z^T of random effect design matrix that relates latent random effects to observations/likelihoods
		* \param Cross_Cov Cross covariance matrix between predicted and obsreved random effects ("=Cov(y_p,y)")
		* \param pred_mean[out] Predicted mean
		* \param pred_cov[out] Predicted covariance matrix
		* \param pred_var[out] Predicted variances
		* \param calc_pred_cov If true, predictive covariance matrix is also calculated
		* \param calc_pred_var If true, predictive variances are also calculated
		* \param calc_mode If true, the mode of the random effects posterior is calculated otherwise the values in mode and a_vec_ are used (default=false)
		* \param only_one_random_effect true if there is only one random effect and ZtZ is diagonal
		*/
		template <typename T1>//T1 can be either den_mat_t or sp_mat_t
		void PredictLAApproxGroupedRE(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data, const sp_mat_t& SigmaI, const sp_mat_t& Zt, T1& Cross_Cov,
			vec_t& pred_mean, T1& pred_cov, vec_t& pred_var, bool calc_pred_cov = false, bool calc_pred_var = false,
			bool calc_mode = false, bool only_one_random_effect = false) {
			if (calc_mode) {// Calculate mode and Cholesky factor of B = (Id + Wsqrt * ZSigmaZt * Wsqrt) at mode
				double mll;//approximate marginal likelihood. This is a by-product that is not used here.
				FindModePostRandEffCalcMLLGroupedRE(y_data, y_data_int, fixed_effects, num_data, SigmaI,
					Zt, mll, only_one_random_effect);
			}
			else {
				CHECK(mode_has_been_calculated_);
			}
			pred_mean = Cross_Cov * first_deriv_ll_;
			if (calc_pred_cov || calc_pred_var) {
				T1 Maux, Maux2;
				Maux = Zt * second_deriv_neg_ll_.asDiagonal() * Cross_Cov.transpose();
				// calculate Maux2 = L\(Z^T * second_deriv_neg_ll_.asDiagonal() * Cross_Cov^T)
				if (only_one_random_effect) {
					Maux2 = diag_SigmaI_plus_ZtWZ_.array().sqrt().inverse().matrix().asDiagonal() * Maux;
				}
				else {
					T1 L = chol_facts_SigmaI_plus_ZtWZ_.matrixL();
					CalcLInvH(L, Maux, Maux2, true);
				}
				if (calc_pred_cov) {
					pred_cov += Maux2.transpose() * Maux2 - (T1)(Cross_Cov * second_deriv_neg_ll_.asDiagonal() * Cross_Cov.transpose());
				}
				if (calc_pred_var) {
					T1 Maux3 = Cross_Cov.cwiseProduct(Cross_Cov * second_deriv_neg_ll_.asDiagonal());
					Maux2 = Maux2.cwiseProduct(Maux2);
#pragma omp parallel for schedule(static)
					for (int i = 0; i < (int)pred_mean.size(); ++i) {
						pred_var[i] += Maux2.col(i).sum() - Maux3.row(i).sum();
					}
				}
			}
		}//end PredictLAApproxGroupedRE


		/*!
		* \brief Make predictions for the (latent) random effects when using the Laplace approximation.
		*		Calculations are done by inverting (Sigma^-1 + W) where it is assumed that an approximate Cholesky factor of Sigma^-1 has
		*		previously been calculated using a Vecchia approximation.
		*		This version is used for the Laplace approximation when there are only GP radnom effects and the Vecchia approximation is used.
		* \param y_data Response variable data if response variable is continuous
		* \param y_data_int Response variable data if response variable is integer-valued (only one of these two is used)
		* \param fixed_effects Fixed effects component of location parameter
		* \param num_data Number of data points
		* \param B Matrix B in Vecchia approximation Sigma^-1 = B^T D^-1 B ("=" Cholesky factor)
		* \param D_inv Diagonal matrix D^-1 in Vecchia approximation Sigma^-1 = B^T D^-1 B
		* \param Cross_Cov Cross covariance matrix between predicted and obsreved random effects ("=Cov(y_p,y)")
		* \param pred_mean[out] Predicted mean
		* \param pred_cov[out] Predicted covariance matrix
		* \param pred_var[out] Predicted variances
		* \param calc_pred_cov If true, predictive covariance matrix is also calculated
		* \param calc_pred_var If true, predictive variances are also calculated
		* \param calc_mode If true, the mode of the random effects posterior is calculated otherwise the values in mode and a_vec_ are used (default=false)
		*/
		template <typename T1>//T1 can be either den_mat_t or sp_mat_t
		void PredictLAApproxVecchia(const double* y_data, const int* y_data_int,
			const double* fixed_effects, const data_size_t num_data, const sp_mat_t& B, const sp_mat_t& D_inv, T1& Cross_Cov,
			vec_t& pred_mean, T1& pred_cov, vec_t& pred_var, bool calc_pred_cov = false, bool calc_pred_var = false, bool calc_mode = false) {
			if (calc_mode) {// Calculate mode and Cholesky factor of Sigma^-1 + W at mode
				double mll;//approximate marginal likelihood. This is a by-product that is not used here.
				FindModePostRandEffCalcMLLVecchia(y_data, y_data_int, fixed_effects, num_data, B, D_inv, mll);
			}
			else {
				CHECK(mode_has_been_calculated_);
			}
			pred_mean = Cross_Cov * first_deriv_ll_;
			if (calc_pred_cov || calc_pred_var) {
				T1 SigmaI_CrossCovT = B.transpose() * D_inv * B * Cross_Cov.transpose();
				// calculate Maux = L\(Sigma^-1 * Cross_Cov^T), L = Chol(Sigma^-1 + W)
				T1 Maux;
				sp_mat_t L = chol_facts_SigmaI_plus_ZtWZ_.matrixL();
				CalcLInvH(L, SigmaI_CrossCovT, Maux, true);
				if (calc_pred_cov) {
					pred_cov += -Cross_Cov * SigmaI_CrossCovT + Maux.transpose() * Maux;
				}
				if (calc_pred_var) {
					Maux = Maux.cwiseProduct(Maux);
#pragma omp parallel for schedule(static)
					for (int i = 0; i < (int)pred_mean.size(); ++i) {
						pred_var[i] += Maux.col(i).sum() - (Cross_Cov.row(i)).dot(SigmaI_CrossCovT.col(i));
					}
				}
			}
		}//end PredictLAApproxVecchia


		/*!
		* \brief Make predictions for the response variable (label) based on predictions for the mean and variance of the latent random effects
		* \param pred_mean[out] Predicted mean of latent random effects. The predicted mean for the response variables is written on this
		* \param pred_var[out] Predicted variances of latent random effects. The predicted variance for the response variables is written on this
		* \param predict_var If true, predictive variances are also calculated
		*/
		void PredictResponse(vec_t& pred_mean, vec_t& pred_var, bool predict_var = false) {
			if (likelihood_type_ == "bernoulli_probit") {
#pragma omp parallel for schedule(static)
				for (int i = 0; i < (int)pred_mean.size(); ++i) {
					pred_mean[i] = normalCDF(pred_mean[i] / std::sqrt(1. + pred_var[i]));
				}
				if (predict_var) {
#pragma omp parallel for schedule(static)
					for (int i = 0; i < (int)pred_mean.size(); ++i) {
						pred_var[i] = pred_mean[i] * (1. - pred_mean[i]);
					}
				}
			}
			else if (likelihood_type_ == "bernoulli_logit") {
#pragma omp parallel for schedule(static)
				for (int i = 0; i < (int)pred_mean.size(); ++i) {
					pred_mean[i] = RespMeanAdaptiveGHQuadrature(pred_mean[i], pred_var[i]);
				}
				if (predict_var) {
#pragma omp parallel for schedule(static)
					for (int i = 0; i < (int)pred_mean.size(); ++i) {
						pred_var[i] = pred_mean[i] * (1. - pred_mean[i]);
					}
				}
			}
			else if (likelihood_type_ == "poisson") {
#pragma omp parallel for schedule(static)
				for (int i = 0; i < (int)pred_mean.size(); ++i) {
					double pm = RespMeanAdaptiveGHQuadrature(pred_mean[i], pred_var[i]);
					if (predict_var) {
						double psm = RespMeanAdaptiveGHQuadrature(2 * pred_mean[i], 4 * pred_var[i]);
						pred_var[i] = psm - pm * pm + pm;
					}
					pred_mean[i] = pm;
				}
			}
			else if (likelihood_type_ == "gamma") {
#pragma omp parallel for schedule(static)
				for (int i = 0; i < (int)pred_mean.size(); ++i) {
					double pm = RespMeanAdaptiveGHQuadrature(pred_mean[i], pred_var[i]);
					if (predict_var) {
						double psm = RespMeanAdaptiveGHQuadrature(2 * pred_mean[i], 4 * pred_var[i]);
						pred_var[i] = psm - pm * pm + psm / aux_pars_[0];
					}
					pred_mean[i] = pm;
				}
			}
		}

		/*!
		* \brief Adaptive GH quadrature to calculate predictive mean of response variable
		* \param latent_mean Predicted mean of latent random effects
		* \param latent_var Predicted variances of latent random effects
		*/
		double RespMeanAdaptiveGHQuadrature(const double latent_mean, const double latent_var) {
			// Find mode of integrand
			double mode_integrand, mode_integrand_last, update;
			mode_integrand = 0.;
			double sigma2_inv = 1. / latent_var;
			double sqrt_sigma2_inv = std::sqrt(sigma2_inv);
			for (int it = 0; it < 100; ++it) {
				mode_integrand_last = mode_integrand;
				update = (FirstDerivLogCondMeanLikelihood(mode_integrand) - sigma2_inv * (mode_integrand - latent_mean))
					/ (SecondDerivLogCondMeanLikelihood(mode_integrand) - sigma2_inv);
				mode_integrand -= update;
				if (std::abs(update) / std::abs(mode_integrand_last) < DELTA_REL_CONV_) {
					break;
				}
			}
			// Adaptive GH quadrature
			double sqrt2_sigma_hat = M_SQRT2 / std::sqrt(-SecondDerivLogCondMeanLikelihood(mode_integrand) + sigma2_inv);
			double x_val;
			double mean_resp = 0.;
			for (int j = 0; j < order_GH_; ++j) {
				x_val = sqrt2_sigma_hat * GH_nodes_[j] + mode_integrand;
				mean_resp += adaptive_GH_weights_[j] * CondMeanLikelihood(x_val) * normalPDF(sqrt_sigma2_inv * (x_val - latent_mean));
			}
			mean_resp *= sqrt2_sigma_hat * sqrt_sigma2_inv;
			return mean_resp;

			////non-adaptive GH quadrature
			//double mean_resp = 0.;
			//double sigma = std::sqrt(latent_var);
			//for (int j = 0; j < order_GH_; ++j) {
			//	mean_resp += GH_weights_[j] * CondMeanLikelihood(M_SQRT2 * sigma * GH_nodes_[j] + latent_mean);
			//}
			//pred_mean *=  M_1_SQRTPI_;
		}

		template <typename T>//T can be double or float
		bool AreSame(const T a, const T b) const {
			return fabs(a - b) < a * EPSILON_;
		}

		// Used for likelihood_type_ == "bernoulli_probit"
		inline double normalCDF(double value) const {
			return 0.5 * std::erfc(-value * M_SQRT1_2);
		}

		inline double normalPDF(double value) const {
			return std::exp(-value * value / 2) / M_SQRT2PI_;
			//return std::exp(-value * value / 2) / std::sqrt(2 * M_PI);
		}

	private:
		/*! \brief Number of data points */
		data_size_t num_data_;
		/*! \brief Number (dimension) of random effecst */
		data_size_t num_re_;
		/*! \brief Posterior mode used for Laplace approximation */
		vec_t mode_;
		/*! \brief Auxiliary variable a=ZSigmaZt^-1 mode_b used for Laplace approximation */
		vec_t a_vec_;
		/*! \brief First derivatives of the log-likelihood */
		vec_t first_deriv_ll_;
		/*! \brief Second derivatives of the negative log-likelihood (diagonal of matrix "W") */
		vec_t second_deriv_neg_ll_;
		/*! \brief Diagonal of matrix Sigma^-1 + Zt * W * Z in Laplace approximation (used only in version 'GroupedRE' when there is only one random effect and ZtWZ is diagonal. Otherwise 'diag_SigmaI_plus_ZtWZ_' is used for grouped REs) */
		vec_t diag_SigmaI_plus_ZtWZ_;
		/*! \brief Cholesky factors of matrix Sigma^-1 + Zt * W * Z in Laplace approximation (used only in versions 'Vecchia' and 'GroupedRE'. For grouped REs, this is used if there is more than one random effect) */
		chol_sp_mat_t chol_facts_SigmaI_plus_ZtWZ_;
		/*! \brief Cholesky factors of matrix I + Wsqrt *  Z * Sigma * Zt * Wsqrt in Laplace approximation (used only in version 'Stable' i.e. neither only grouped REs nor Vecchia approximation) */
		T2 chol_facts_Id_plus_Wsqrt_ZSigmaZt_Wsqrt_;
		/*! \brief If true, the mode has been initialized to 0 */
		bool mode_initialized_ = false;
		/*! \brief If true, the mode has been determined */
		bool mode_has_been_calculated_ = false;
		/*! \brief If true, the function 'CheckY' has been called */
		bool normalizing_constant_has_been_calculated_ = false;
		/*! \brief Normalizing constant for likelihoods (not all likelihoods have one) */
		double log_normalizing_constant_;

		/*! \brief Type of likelihood  */
		string_t likelihood_type_ = "gaussian";
		/*! \brief List of supported covariance likelihoods */
		const std::set<string_t> SUPPORTED_LIKELIHOODS_{ "gaussian", "bernoulli_probit", "bernoulli_logit", "poisson", "gamma" };
		/*! \brief Tolerance level when comparing two doubles for equality */
		double EPSILON_ = 1e-6;
		/*! \brief Maximal number of iteration done for finding posterior mode with Newton's method */
		int MAXIT_MODE_NEWTON_ = 100;
		/*! \brief Used for cheking convergence in mode finding algorithm (terminate if relative change in Laplace approx. is below this value) */
		double DELTA_REL_CONV_ = 1e-6;
		/*! \brief Additional parameters for likelihoods. For gamma, auxiliary_pars_[0] = shape parameter  */
		std::vector<double> aux_pars_;

		string_t ParseLikelihoodAlias(const string_t& likelihood) {
			if (likelihood == string_t("binary") || likelihood == string_t("bernoulli_probit") || likelihood == string_t("binary_probit")) {
				return "bernoulli_probit";
			}
			else if (likelihood == string_t("gaussian") || likelihood == string_t("regression")) {
				return "gaussian";
			}
			return likelihood;
		}

		//Derived constants not defined in cmath
		//1/sqrt(2*pi)
		const double M_SQRT2PI_ = std::sqrt(2. * M_PI);
		////1/sqrt(pi) (not used anymore, used for non-adaptive GH quadrature)
		//const double M_1_SQRTPI_ = M_2_SQRTPI / 2.;

		/*! \brief Order of the Gauss-Hermite quadrature */
		int order_GH_ = 30;
		/*! \brief Nodes and weights for the Gauss-Hermite quadrature */
		// Source: https://keisan.casio.com/exec/system/1281195844
		const std::vector<double> GH_nodes_ = { -6.863345293529891581061,
										-6.138279220123934620395,
										-5.533147151567495725118,
										-4.988918968589943944486,
										-4.48305535709251834189,
										-4.003908603861228815228,
										-3.544443873155349886925,
										-3.099970529586441748689,
										-2.667132124535617200571,
										-2.243391467761504072473,
										-1.826741143603688038836,
										-1.415527800198188511941,
										-1.008338271046723461805,
										-0.6039210586255523077782,
										-0.2011285765488714855458,
										0.2011285765488714855458,
										0.6039210586255523077782,
										1.008338271046723461805,
										1.415527800198188511941,
										1.826741143603688038836,
										2.243391467761504072473,
										2.667132124535617200571,
										3.099970529586441748689,
										3.544443873155349886925,
										4.003908603861228815228,
										4.48305535709251834189,
										4.988918968589943944486,
										5.533147151567495725118,
										6.138279220123934620395,
										6.863345293529891581061 };
		const std::vector<double> GH_weights_ = { 2.908254700131226229411E-21,
										2.8103336027509037088E-17,
										2.87860708054870606219E-14,
										8.106186297463044204E-12,
										9.1785804243785282085E-10,
										5.10852245077594627739E-8,
										1.57909488732471028835E-6,
										2.9387252289229876415E-5,
										3.48310124318685523421E-4,
										0.00273792247306765846299,
										0.0147038297048266835153,
										0.0551441768702342511681,
										0.1467358475408900997517,
										0.2801309308392126674135,
										0.386394889541813862556,
										0.3863948895418138625556,
										0.2801309308392126674135,
										0.1467358475408900997517,
										0.0551441768702342511681,
										0.01470382970482668351528,
										0.002737922473067658462989,
										3.48310124318685523421E-4,
										2.938725228922987641501E-5,
										1.579094887324710288346E-6,
										5.1085224507759462774E-8,
										9.1785804243785282085E-10,
										8.10618629746304420399E-12,
										2.87860708054870606219E-14,
										2.81033360275090370876E-17,
										2.9082547001312262294E-21 };
		const std::vector<double> adaptive_GH_weights_ = { 0.83424747101276179534,
										0.64909798155426670071,
										0.56940269194964050397,
										0.52252568933135454964,
										0.491057995832882696506,
										0.46837481256472881677,
										0.45132103599118862129,
										0.438177022652683703695,
										0.4279180629327437485828,
										0.4198950037368240886418,
										0.413679363611138937184,
										0.4089815750035316024972,
										0.4056051233256844363121,
										0.403419816924804022553,
										0.402346066701902927115,
										0.4023460667019029271154,
										0.4034198169248040225528,
										0.4056051233256844363121,
										0.4089815750035316024972,
										0.413679363611138937184,
										0.4198950037368240886418,
										0.427918062932743748583,
										0.4381770226526837037,
										0.45132103599118862129,
										0.46837481256472881677,
										0.4910579958328826965056,
										0.52252568933135454964,
										0.56940269194964050397,
										0.64909798155426670071,
										0.83424747101276179534 };
	};

}  // namespace GPBoost

#endif   // GPB_LIKELIHOODS_