// Copyright 2015 The Edda Authors. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

#ifndef JointGMM_H
#define JointGMM_H

#include <iostream>
#include <typeinfo>
#include <iostream>

#include "common.h"
#include "edda_export.h"
#include "core/vector_matrix.h"
#include "core/tuple.h"
#include "core/thrust_common.h"
#include "distribution_tag.h"
#include "core/shared_ary.h"
#include "distributions/joint_gaussian.h"

#include "common.h"
#include "distribution_tag.h"
#include "core/statistics.h"

#include <boost/numeric/ublas/matrix.hpp>
#include <boost/numeric/ublas/io.hpp>
#include <boost/numeric/ublas/assignment.hpp> 
//using namespace boost::numeric::ublas;

#define EMEPS 0.0000000000222044604

namespace edda {
	namespace dist {

		///
		/// \brief The Distribution class is a root class for all distribution-type classes.
		///
		/// This is useful for applications to identify whether a class is a distribution-type class, by using ENABLE_IF_BASE_OF()
		///
		class EDDA_EXPORT JointGMM : public ContinuousDistributionTag, public JointDistributionTag {
		public:

			///
			/// \brief Constructor
			///
			JointGMM(){}

			///
			/// \brief Constructor by giving GMM's parameters
			///
			JointGMM(const JointGMM &gmm)
				: weights(gmm.weights),	gaus(gmm.gaus),	nVar(gmm.nVar),	nComp(gmm.nComp)
			{
			}

			///
			/// \brief Return probability of x
			///
			void setGMM(int _nVar, int _nComp, ublas_vector &_weights, ublas_matrix &_means, ublas_matrix &_covs){
				nVar = _nVar;
				nComp = _nComp;

				gaus.clear();
				weights.resize(nComp);
				std::copy(_weights.begin(), _weights.end(), weights.begin());

				for (int i = 0; i < nComp; i++){
					boost::numeric::ublas::matrix_row<boost::numeric::ublas::matrix<float> > m(_means, i);
					ublas_matrix cov_ = subrange( _covs, i*nVar, (i+1)*nVar, 0, nVar );

					JointGaussian g(m, cov_);
					gaus.push_back(g);
				}
			}

			///
			/// \brief Return means
			///
			std::vector<Real> getMean(int comp) const{
				return getJointMean(gaus[comp]);
			}

			///
			/// \brief Return probability of x
			///
			Real getPdf(const std::vector<Real> x) const{
				Real pdf = 0;
				for (int i = 0; i < nComp; i++){
					pdf += weights[i] * getJointPdf(gaus[i], x);
				}

				return pdf;
			}

			///
			/// \brief Return weight probability of each component (only used by EM)
			///
			std::vector<Real> getCompWgtLogProbability(const std::vector<Real> x) const{
				std::vector<Real> wProbs(nComp, 0);
				for (int i = 0; i < nComp; i++){
					wProbs[i] = log(weights[i]) + gaus[i].getJointLogPdf(x); 
				}

				return wProbs;
			}

			///
			/// \brief Return a samplel drawn from GMM
			///
			std::vector<Real> getJointSample(thrust::default_random_engine &rng) const{
				Real r = rand() / (float)RAND_MAX;
				float sum = 0;
				std::vector<Real> s;

				for (int i = 0; i < gaus.size(); i++){
					sum += weights[i];
					if (sum >= r || i==gaus.size()-1){
						s = gaus[i].getJointSample(rng);
						break;
					}
				}
				return s;
			}

			///
			/// \brief Print GMM parameters
			///
			void output(std::ostream& os) const{
				for (int i = 0; i < nComp; i++){
					os << "Component: " << i << std::endl;
					os << "weight: " << weights[i] << std::endl;
					os << gaus[i] << std::endl;
				}
			}

		protected:
			std::vector<Real> weights;
			std::vector<JointGaussian> gaus;
			int nVar;
			int nComp;
		};

		///
		/// \brief Return probability of x
		///
		inline Real getPdf(const JointGMM &dist, const std::vector<Real> x)
		{
			return (Real)dist.getPdf(x);
		}

		///
		/// \brief Return a sample drawn from dist
		///
		inline std::vector<Real> getJointSample(const JointGMM &dist, thrust::default_random_engine &rng)
		{
			return dist.getJointSample(rng);
		}

		///
		/// \brief Print itself
		///
		inline std::ostream& operator<<(std::ostream& os, const JointGMM &dist)
		{
			dist.output(os);
			return os;
		}

		///
		/// \brief Print the distribution typw
		///
		__host__ __device__
		inline std::string getName(const JointGMM &x) {
			return "JointGMM";
		}

	}  // namespace dist
	
	///
	/// \brief Compute the Gaussian Mixture Model 
	///
	inline dist::JointGMM eddaComputeJointGMM(std::vector<Real*>& dataAry, int nSamples, int nComp)
		//inline dist::JointGMM eddaComputeJointGMM(int nComp, ublas_matrix &samples)
	{
		int nVar = dataAry.size();

		//convert input sample to ublas_matrix
		ublas_matrix samples(nSamples, nVar);
		for (int j = 0; j < nVar; j++){
			for (int i = 0; i < nSamples; i++){
				samples(i, j) = dataAry[j][i];
			}
		}

		dist::JointGMM gmm;
		//initialization: 
		//equally set weights, randomly pickup mean, set identity CovMatrix
		ublas_vector w(nComp);
		ublas_matrix m(nComp, nVar);
		ublas_matrix covs(nComp* nVar, nVar);
		for (int i = 0; i < nComp; i++)w(i) = 1.0/(float)nComp;
		for (int i = 0; i < nComp; i++){
			int r = rand() % nSamples;
			subrange(m, i, i + 1, 0, nVar) = subrange(samples, r, r + 1, 0, nVar);
		}
		for (int i = 0; i < nComp; i++ ){
			subrange(covs, i*nVar, (i+1)*nVar, 0, nVar) = ublas::identity_matrix<Real>(nVar);
		}

		int n_iter = 100;
		float current_log_likelihood = NAN;
		bool converged_ = false;

		ublas_matrix log_likelihoods(nSamples, 1);
		ublas_matrix resposibilities(nSamples, nComp);
		for (int i = 0; i < n_iter; i++){
			gmm.setGMM(nVar, nComp, w, m, covs);
			float prev_log_likelihood = current_log_likelihood;
			//E step
			current_log_likelihood = 0;
			for (int smpPtr = 0; smpPtr < nSamples; smpPtr++){
				std::vector<Real> s(nVar);
				boost::numeric::ublas::matrix_row<boost::numeric::ublas::matrix<float> > mr(samples, smpPtr);
				std::copy(mr.begin(), mr.end(), s.begin());
				std::vector<Real> score = gmm.getCompWgtLogProbability(s);
				
				log_likelihoods(smpPtr, 1) = 0;
				
				double maxScore = score[0];
				for (int j = 1; j < nComp; j++){
					if (score[j] > maxScore)maxScore = score[j];
				}
				for (int j = 0; j < nComp; j++){
					log_likelihoods(smpPtr, 1) += exp(score[j]-maxScore);
				}

				log_likelihoods(smpPtr, 1) = log(log_likelihoods(smpPtr, 1)) + maxScore;
				current_log_likelihood += log_likelihoods(smpPtr, 1);
				for (int j = 0; j < nComp; j++){
					resposibilities(smpPtr, j) = exp(score[j] - log_likelihoods(smpPtr, 1));
				}
			}

			//current log likelihood
			current_log_likelihood /= (float)nSamples;
			
			if (prev_log_likelihood == prev_log_likelihood){
				double change = abs(current_log_likelihood - prev_log_likelihood);
				if (change < 0.001){//threshold for stopping EM
					break;
				}
			}

			//E step
			ublas_vector weights(nComp);
			for (int j = 0; j < nComp; j++){
				weights(j) = 0;
				for (int smpPtr = 0; smpPtr < nSamples; smpPtr++){
					weights(j) += resposibilities(smpPtr, j);
				}
			}
			
			ublas_matrix weighted_X_sum = boost::numeric::ublas::prod(boost::numeric::ublas::trans(resposibilities) , samples);
			
			double sumWeight = 0;
			for (int j = 0; j < nComp; j++){
				sumWeight += weights(j);
			}
			for (int j = 0; j < nComp; j++){
				w(j) = (weights(j) / (sumWeight + 10 * EMEPS) + EMEPS);
			}
			
			for (int c = 0; c < nComp; c++){
				subrange(m, c, c + 1, 0, nVar) = subrange(weighted_X_sum, c, c + 1, 0, nVar)  / (weights(c) + 10 * EMEPS);
			}
			
			//estimate covariance matrix
			for (int c = 0; c < nComp; c++){
				ublas_vector post(nSamples);
				double sumPost = 0;
				for (int s = 0; s < nSamples; s++){
					post(s) = resposibilities(s, c);
					sumPost += resposibilities(s, c);
				}
				ublas_vector mu = row(m, c);
				ublas_matrix diff(nSamples, nVar);
				for (int s = 0; s < nSamples; s++){
					row(diff, s) = row(samples, s) - mu;
				}

				ublas_matrix postDiff( nVar, nSamples );
				for (int s = 0; s < nSamples; s++){
					column(postDiff, s) = row(diff, s) * post(s);
				}
								
				ublas_matrix avg_cv = boost::numeric::ublas::prod(postDiff, diff);
				avg_cv = avg_cv / (sumPost + 10 * EMEPS);
				ublas_matrix cv = avg_cv + 0.001 * ublas::identity_matrix<Real>(nVar);
				subrange(covs, c*nVar, (c + 1)*nVar, 0, nVar) = cv;
			}
		}

		gmm.setGMM(nVar, nComp, w, m, covs);
		return gmm;
	}

}  // namespace edda

#endif // JointGMM_H
