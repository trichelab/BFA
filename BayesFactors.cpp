#include <iostream>
#include <fstream>
#include <stdio.h>
#include <math.h>
#include <omp.h>

#include <random>
#include <map>
#include <string>
#include <iomanip>

#include <unistd.h>
#include <string>
#include <algorithm>
#include <random>

#include <Eigen/Core>
#include <Eigen/Dense>

#include <boost/random.hpp>
#include <boost/math/distributions.hpp>
#include <boost/program_options.hpp>
#include <iterator>
#include <armadillo>

using namespace std;
using namespace Eigen;
using namespace arma;
namespace po = boost::program_options;

using Eigen::Matrix;
using Eigen::MatrixXd;
using Eigen::VectorXd;
using Eigen::LLT;
using Eigen::Lower;
using Eigen::Map;
using Eigen::Upper;
typedef Map<MatrixXd> MapMatd;

boost::random::mt19937 gen(time(0));

//distributions
double runif(double lower, double higher)
{
	boost::random::uniform_real_distribution<> dist(lower, higher);
	return dist(gen);
}

double rnorm(double mean, double sd)
{
	boost::random::normal_distribution<> dist(mean, sd);
	return dist(gen);
}


double rbeta(double alpha, double beta)
{

	boost::math::beta_distribution<> dist(alpha, beta);
	double q = quantile(dist, runif(0,1));

	return(q);
}

double rinvchisq(double df, double scale)
{

	boost::math::inverse_chi_squared_distribution<> dist(df, scale);
	double q = quantile(dist, runif(0,1));

	return(q);
}
int rbernoulli(double p)
{
	std::bernoulli_distribution dist(p);
	return dist(gen);
}

//switch between armadillo and eigen
Eigen::MatrixXd cast_eigen(arma::mat arma_A)
{
	Eigen::MatrixXd eigen_B = Eigen::Map<Eigen::MatrixXd>(arma_A.memptr(),
			arma_A.n_rows,
			arma_A.n_cols);

	return eigen_B;
}

arma::mat cast_arma(Eigen::MatrixXd& eigen_A)
{
	arma::mat arma_B = arma::mat(eigen_A.data(), eigen_A.rows(), eigen_A.cols(),false, false);
	return arma_B;
}

//sampling functions
double sample_hyper(const MatrixXd& w1_M1_sample, const MatrixXd& WI_m, int b0_m, const VectorXd& mu0_m, int df_m,VectorXd& mu_m,MatrixXd& lambda_m)
{
	//Sample from individual or SNP hyperparams (equation 14)
	int N = w1_M1_sample.rows();
	int num_feat=w1_M1_sample.cols();
	VectorXd u_bar(num_feat);
	MatrixXd S_bar(num_feat,num_feat);

	u_bar = w1_M1_sample.colwise().mean();
	//covariance to correlation
	S_bar = (w1_M1_sample.transpose()*w1_M1_sample)/N;

	//Gaussian-Wishard sampling
	//1.sample lambda from wishard distribution (lambda_m or lambda_u)
	//2.sample mu (mu_m or mu_u) from multivariate normal distribution with mean mu_0 (mu_temp) and variance (lambda*Lambda)^-1

	//Wishard-covariance matrix
	MatrixXd WI_post(num_feat,num_feat);
	WI_post = WI_m.inverse() + N*S_bar+((N*b0_m)/(b0_m+N))*((mu0_m - u_bar)*(mu0_m - u_bar).transpose());
	//WI_post = (WI_post + WI_post.transpose())/2;

	WI_post=WI_post.inverse();

	//Wishard-degrees of freedom
	int df_mpost = df_m+N;
	//Wishard draw, using the armadillo function. So I cast to an armadillo matrix, draw wishart then cast back to eigen matrix.
	//should just code the wishart function.
	mat arma_lambda =cast_arma(WI_post);
	lambda_m=cast_eigen(wishrnd(arma_lambda, df_mpost));

	//multivariate normal mean
	VectorXd mu_temp(num_feat);
	mu_temp = (b0_m*mu0_m + N*u_bar)/(b0_m+N);

	//Multivariate normal with mean mu_temp and cholesky decomposed covariance lam
	MatrixXd lam = (((b0_m+N)*lambda_m).inverse());

	lam=lam.llt().matrixU();
	lam.transposeInPlace();

	MatrixXd normV(num_feat,1);
	for (int i=0;i<num_feat;i++){
		normV(i,0)=rnorm(0,1);
	}

	mu_m = lam*normV+mu_temp;

	return 0;
}

double sample_ind (const MatrixXd& w1_M1_sample, MatrixXd& w1_P1_sample, const MatrixXd& X,int num_p,int num_feat,const MatrixXd& lambda_u,const VectorXd& mu_u,double alpha)
{
	//random shuffling of individuals.
	std::vector<int> I;
	for (int i=0; i<num_p; ++i) {
		I.push_back(i);
	}

	std::random_shuffle(I.begin(), I.end());

	// Gibbs updates over individual latent vectors given hyperparams.
	// Infer posterior distribution over individual latent vectors (equation 11).

	for (int i=0;i<num_p;i++){
		//observed data row
		VectorXd rr = X.row(I[i]);
		//equation 12
		MatrixXd covar = ((alpha*(w1_M1_sample.transpose()*w1_M1_sample)+lambda_u));
		covar=covar.inverse();
		//equation 13
		VectorXd mean_u = covar * (alpha*w1_M1_sample.transpose()*rr+lambda_u*mu_u);
		//multivariate normal with mean mean_u and cholesky decomposed variance lam
		MatrixXd lam = covar.llt().matrixU();
		lam.transposeInPlace();

		MatrixXd normV(num_feat,1);
		for (int j=0;j<num_feat;j++){
			normV(j,0)=rnorm(0,1);
		}

		w1_P1_sample.row(I[i]) = (lam*normV+mean_u).transpose();

	}

	return 0;
}

double sample_SNP (const MatrixXd& w1_P1_sample, MatrixXd& w1_M1_sample, const MatrixXd& X,int num_p,int num_feat,const MatrixXd& lambda_m,const VectorXd& mu_m,double alpha)
{
	//random shuffling of markers.
	std::vector<int> I;
	for (int i=0; i<num_p; ++i) {
		I.push_back(i);
	}

	std::random_shuffle(I.begin(), I.end());

	// Gibbs updates over SNP latent vectors given hyperparams.
	// Infer posterior distribution over SNP latent vectors (equation 11).

	for (int i=0;i<num_p;i++){
		//observed data column
		VectorXd rr = X.col(I[i]);
		//equation 12
		MatrixXd covar = ((alpha*(w1_P1_sample.transpose()*w1_P1_sample)+lambda_m));
		covar=covar.inverse();
		//equation 13
		VectorXd mean_m = covar * (alpha*w1_P1_sample.transpose()*rr+lambda_m*mu_m);
		//multivariate normal with mean mean_m and cholesky decomposed variance lam
		MatrixXd lam = covar.llt().matrixU();
		lam.transposeInPlace();

		MatrixXd normV(num_feat,1);
		for (int j=0;j<num_feat;j++){
			normV(j,0)=rnorm(0,1);
		}
		w1_M1_sample.row(I[i]) = (lam*normV+mean_m).transpose();
	}

	return 0;
}


int main(int argc, char *argv[])
{

	po::options_description desc("Options");
	desc.add_options()
				("M", po::value<int>()->required(), "No. of markers")
				("N", po::value<int>()->required(), "No. of individuals")
				("num_feat", po::value<int>()->default_value(3), "No. of factors")
				("iter", po::value<int>()->default_value(100), "No. of Gibbs iterations")
				("input", po::value<std::string>()->required(),"Input filename")
				("out", po::value<std::string>()->default_value("BayesFactors_out"),"Output filename");

	srand(time(0));

	po::variables_map vm;
	po::store(po::parse_command_line(argc,argv,desc),vm);
	po::notify(vm);

	int M=vm["M"].as<int>();
	int N=vm["N"].as<int>();
	int num_feat=vm["num_feat"].as<int>();
	int iter=vm["iter"].as<int>();
	string input=vm["input"].as<string>();
	string output=vm["out"].as<string>();

	MatrixXd X(N,M);

	int i,j,k,l,m=0;
	auto timenow = chrono::system_clock::to_time_t(chrono::system_clock::now());
	cout<<"Started analysis!"<<endl;
	timenow = chrono::system_clock::to_time_t(chrono::system_clock::now());
	cout << ctime(&timenow) << endl;

	ifstream f1(input+".X");
	if (f1){
		for (int i = 0; i < N; i++)
		{
			for (int j = 0; j < M; j++)
			{
				f1 >> X(i,j);
				//cout<<X(i,j)<<endl;
			}
		}
		f1.close();
		cout<<"finished reading matrix X!"<<endl;
		timenow = chrono::system_clock::to_time_t(chrono::system_clock::now());
		cout << ctime(&timenow) << endl;
	}else{
		cout<<"the "+input+".X"+" file does not exist/cannot be opened!"<<endl;
		return 0;
	}
	//normalize matrix X (genotype matrix)
	RowVectorXd mean = X.colwise().mean();
	RowVectorXd sd = (X.rowwise() - mean).array().square().colwise().mean();
	X = (X.rowwise() - mean).array().rowwise() / sd.array();

	//Factor analysis
	//Initialization of latent variables. It can be done with ML estimates. Here its done just by sampling normal deviates.
	MatrixXd w1_M1_sample(M,num_feat);
	for (int i = 0; i < M; i++)
	{
		for (int j = 0; j < num_feat; j++)
		{
			w1_M1_sample(i,j)=rnorm(0,1);
		}

	}
	MatrixXd w1_P1_sample(N,num_feat);
	for (int i = 0; i < N; i++)
	{
		for (int j = 0; j < num_feat; j++)
		{
			w1_P1_sample(i,j)=rnorm(0,1);
		}

	}
	//Initialization of hyperparameters
	MatrixXd WI_u(num_feat,num_feat);
	VectorXd mu_u(num_feat);
	MatrixXd lambda_u(num_feat,num_feat);
	VectorXd mu0_u(num_feat);

	MatrixXd WI_m(num_feat,num_feat);
	VectorXd mu_m(num_feat);
	MatrixXd lambda_m(num_feat,num_feat);
	VectorXd mu0_m(num_feat);

	WI_m.setIdentity();
	mu_m.setZero();
	lambda_m.setIdentity();
	mu0_m.setZero();

	WI_u.setIdentity();
	mu_u.setZero();
	lambda_u.setIdentity();
	mu0_u.setZero();

	int b0_m=2;int df_m=num_feat;
	int b0_u=2;int df_u=num_feat;

	double alpha=1;

	//create folder to put output
	int systemRet = system("mkdir -p BayesFactors_out");
	if(systemRet == -1){
		cout<<"system command to create folder to place output FAILED!"<<endl;
	}

	//declare filestreams for output
	ofstream file_lambda_u;
	ofstream file_lambda_m;
	ofstream file_latentInd;
	ofstream file_latentSNPs;

	//apending hyper-parameters
	file_lambda_m.open ("BayesFactors_out/"+output+"_lambda_m.txt", std::ios_base::app);
	file_lambda_u.open ("BayesFactors_out/"+output+"_lambda_u.txt", std::ios_base::app);

	//begin Gibbs updates
	for (int i = 0; i < iter+1; i++)
	{
		//write out for each iteration the factors
		file_latentInd.open ("BayesFactors_out/"+output+".iter"+to_string(i)+".factors");
		file_latentInd << w1_P1_sample << ' ';

		file_latentInd << endl;
		file_latentInd.close();

		//write out for each iteration the scores
		file_latentSNPs.open ("BayesFactors_out/"+output+".iter"+to_string(i)+".scores");
		file_latentSNPs << w1_M1_sample << ' ';

		file_latentSNPs<< endl;
		file_latentSNPs.close();

		//write-out hyperparameters (covariance matrices)
		//file_lambda_m << i <<" ";
		for (int j = 0; j < lambda_m.rows(); j++){
		file_lambda_m << lambda_m.row(j) << " ";
		}
		file_lambda_m<<endl;
		//file_lambda_u << i<<" ";
		for (int j = 0; j <lambda_u.rows(); j++){
		file_lambda_u  << lambda_u.row(j) << " ";
		}
		file_lambda_u<<endl;

		//GIBBS UPDATES

		//update SNP hyperparameters
		sample_hyper(w1_M1_sample,WI_m,b0_m,mu0_m,df_m,mu_m,lambda_m);
		//update individual hyperparameters
		sample_hyper(w1_P1_sample,WI_u,b0_u,mu0_u,df_u,mu_u,lambda_u);
		//update individual parameters
		sample_ind (w1_M1_sample,w1_P1_sample,X,N,num_feat,lambda_u,mu_u,alpha);
		//update SNP parameters
		sample_SNP (w1_P1_sample,w1_M1_sample,X,M,num_feat,lambda_m,mu_m,alpha);

	}
	file_lambda_u.close();
	file_lambda_m.close();

	cout<<"Finished!"<<endl;
	timenow = chrono::system_clock::to_time_t(chrono::system_clock::now());
	cout << ctime(&timenow) << endl;

	return 0;
}
