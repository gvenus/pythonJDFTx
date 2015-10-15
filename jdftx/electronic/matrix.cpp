/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <core/Random.h>
#include <core/BlasExtra.h>
#include <core/GpuUtil.h>
#include <electronic/matrix.h>

//---------------------- class diagMatrix --------------------------

bool diagMatrix::isScalar(double absTol, double relTol) const
{	double mean = 0.0; for(double d: *this) mean += d; mean /= nRows();
	double errThresh = fabs(absTol) + fabs(mean * relTol);
	for(double d: *this) if(fabs(d-mean) > errThresh) return false;
	return true;
}

diagMatrix diagMatrix::operator()(int iStart, int iStop) const
{	myassert(iStart>=0 && iStart<nRows());
	myassert(iStop>iStart && iStop<=nRows());
	int iDelta = iStop-iStart;
	diagMatrix ret(iDelta);
	for(int i=0; i<iDelta; i++) ret[i] = at(i+iStart);
	return ret;
}
void diagMatrix::set(int iStart, int iStop, const diagMatrix& m)
{	myassert(iStart>=0 && iStart<nRows());
	myassert(iStop>iStart && iStop<=nRows());
	int iDelta = iStop-iStart;
	myassert(iDelta==m.nRows());
	for(int i=0; i<iDelta; i++) at(i+iStart) = m[i];
}
diagMatrix diagMatrix::operator()(int iStart, int iStep,  int iStop) const
{	myassert(iStart>=0 && iStart<nRows());
	myassert(iStop>iStart && iStop<=nRows());
	myassert(iStep>0);
	int iDelta = ceildiv(iStop-iStart, iStep);
	diagMatrix ret(iDelta);
	for(int i=0; i<iDelta; i++) ret[i] = at(i*iStep+iStart);
	return ret;
}
void diagMatrix::set(int iStart, int iStep, int iStop, const diagMatrix& m)
{	myassert(iStart>=0 && iStart<nRows());
	myassert(iStop>iStart && iStop<=nRows());
	myassert(iStep>0);
	int iDelta = ceildiv(iStop-iStart, iStep);
	myassert(iDelta==m.nRows());
	for(int i=0; i<iDelta; i++) at(i*iStep+iStart) = m[i];
}

void diagMatrix::scan(FILE* fp)
{	for(double& d: *this) fscanf(fp, "%lg", &d);
}

void diagMatrix::print(FILE* fp, const char* fmt) const
{	for(double d: *this) fprintf(fp, fmt, d);
	fprintf(fp,"\n");
}

void diagMatrix::send(int dest, int tag) const
{	myassert(mpiUtil->nProcesses()>1);
	mpiUtil->send(data(), size(), dest, tag);
}
void diagMatrix::recv(int src, int tag)
{	myassert(mpiUtil->nProcesses()>1);
	mpiUtil->recv(data(), size(), src, tag);
}
void diagMatrix::bcast(int root)
{	if(mpiUtil->nProcesses()>1)
		mpiUtil->bcast(data(), size(), root);
}
void diagMatrix::allReduce(MPIUtil::ReduceOp op, bool safeMode)
{	if(mpiUtil->nProcesses()>1)
		mpiUtil->allReduce(data(), size(), op, safeMode);
}

//----------------------- class matrix ---------------------------

//Initialization
void matrix::init(int nrows, int ncols, bool onGpu)
{
	nr = nrows;
	nc = ncols;
	
	if(nr*nc>0) memInit("matrix", nr*nc, onGpu);
}
//Reshaping
void matrix::reshape(int nrows, int ncols)
{	myassert(nrows>=0);
	myassert(ncols>=0);
	size_t nProd = nr * nc; //current size
	//Fill in missing dimensions if any:
	if(!nrows) { myassert(ncols); nrows = nProd / ncols; }
	if(!ncols) { myassert(nrows); ncols = nProd / nrows; }
	//Update dimensions:
	myassert(nrows * ncols == int(nProd));
	nr = nrows;
	nc = ncols;
}
// Default constructor
matrix::matrix(int nrows, int ncols, bool onGpu)
{	init(nrows,ncols, onGpu);
}
// Copy constructor
matrix::matrix(const matrix& m1)
{	init(m1.nRows(), m1.nCols(), m1.isOnGpu());
	memcpy((ManagedMemory&)*this, (const ManagedMemory&)m1);
}
// Move constructor
matrix::matrix(matrix&& m1)
{	std::swap(nr, m1.nr);
	std::swap(nc, m1.nc);
	memMove((ManagedMemory&&)m1);
}
// Construct from a real diagonal matrix:
matrix::matrix(const diagMatrix& d)
{	nr = d.size();
	nc = d.size();
	if(d.size())
	{	memInit("matrix", nr*nc); zero();
		complex* thisData = data();
		for(int i=0; i<nRows(); i++) thisData[index(i,i)] = d[i];
	}
}
// Construct from a complex diagonal matrix:
matrix::matrix(const std::vector<complex>& d)
{	nr = d.size();
	nc = d.size();
	if(d.size())
	{	memInit("matrix", nr*nc); zero();
		complex* thisData = data();
		for(int i=0; i<nRows(); i++) thisData[index(i,i)] = d[i];
	}
}
// Construct from a complex diagonal matrix:
matrix::matrix(const matrix3<>& m)
{	nr = 3;
	nc = 3;
	memInit("matrix", nr*nc);
	for(int j=0; j<3; j++)
		for(int i=0; i<3; i++)
			set(i,j, m(i,j));
}
//Copy assignment
matrix& matrix::operator=(const matrix &m1)
{	init(m1.nRows(), m1.nCols(), m1.isOnGpu());
	memcpy((ManagedMemory&)*this, (const ManagedMemory&)m1);
	return *this;
}
// Move assignment
matrix& matrix::operator=(matrix&& m1)
{	std::swap(nr, m1.nr);
	std::swap(nc, m1.nc);
	memMove((ManagedMemory&&)m1);
	return *this;
}

//------------- Sub-matrices ------------------------

complex matrix::operator()(int i, int j) const
{	myassert(i<nr and i>=0);
	myassert(j<nc and j>=0);
	if(isOnGpu())
	{	complex ret;
		#ifdef GPU_ENABLED
		cudaMemcpy(&ret, dataGpu()+index(i,j), sizeof(complex), cudaMemcpyDeviceToHost);
		#else
		myassert(!"onGpu=true without GPU_ENABLED");
		#endif
		return ret;
	}
	else return data()[index(i,j)];
}

#ifdef GPU_ENABLED
void matrixSubGet_gpu(int nr, int iStart, int iStep, int iDelta, int jStart, int jStep, int jDelta, const complex* in, complex* out); //implemented in operators.cu
#endif
matrix matrix::operator()(int iStart, int iStep,  int iStop, int jStart, int jStep, int jStop) const
{	if(iStart==0 && iStep==1 && iStop==nr && jStart==0 && jStep==1 && jStop==nc)
		return *this; //faster to copy matrix for this special case
	
	myassert(iStart>=0 && iStart<nr);
	myassert(iStop>iStart && iStop<=nr);
	myassert(iStep>0);
	myassert(jStart>=0 || jStart<nc);
	myassert(jStop>jStart || jStop<=nc);
	myassert(jStep>0);
	
	int iDelta = ceildiv(iStop-iStart, iStep), jDelta = ceildiv(jStop-jStart, jStep);
	matrix ret(iDelta,jDelta, isGpuEnabled()); complex* retData = ret.dataPref(); const complex* thisData = this->dataPref();
	#ifdef GPU_ENABLED
	matrixSubGet_gpu(nr, iStart,iStep,iDelta, jStart,jStep,jDelta, thisData, retData);
	#else
	for(int i=0; i<iDelta; i++)
		for(int j=0; j<jDelta; j++)
			retData[ret.index(i,j)] = thisData[this->index(i*iStep+iStart, j*jStep+jStart)];
	#endif
	return ret;
}


void matrix::set(int i, int j, complex m)
{	myassert(i<nr and i>=0);
	myassert(j<nc and j>=0);
	if(isOnGpu())
	{
		#ifdef GPU_ENABLED
		cudaMemcpy(dataGpu()+index(i,j), &m, sizeof(complex), cudaMemcpyHostToDevice);
		#else
		myassert(!"onGpu=true without GPU_ENABLED");
		#endif
	}
	else data()[index(i,j)] = m;
}

#ifdef GPU_ENABLED
void matrixSubSet_gpu(int nr, int iStart, int iStep, int iDelta, int jStart, int jStep, int jDelta, const complex* in, complex* out); //implemented in operators.cu
#endif
void matrix::set(int iStart, int iStep, int iStop, int jStart, int jStep, int jStop, const matrix& m)
{	myassert(iStart>=0 && iStart<nr);
	myassert(iStop>iStart && iStop<=nr);
	myassert(iStep>0);
	myassert(jStart>=0 || jStart<nc);
	myassert(jStop>jStart || jStop<=nc);
	myassert(jStep>0);
	int iDelta = ceildiv(iStop-iStart, iStep), jDelta = ceildiv(jStop-jStart, jStep);
	myassert(iDelta==m.nr);
	myassert(jDelta==m.nc);

	const complex* mData = m.dataPref(); complex* thisData = this->dataPref();
	#ifdef GPU_ENABLED
	matrixSubSet_gpu(nr, iStart,iStep,iDelta, jStart,jStep,jDelta, mData, thisData);
	#else
	for(int i=0; i<iDelta; i++)
		for(int j=0; j<jDelta; j++)
			thisData[this->index(i*iStep+iStart, j*jStep+jStart)] = mData[m.index(i,j)];
	#endif
}

//----------- Formatted (ascii) read/write ----------

void matrix::scan(FILE* fp, const char* fmt)
{	complex* thisData = this->data();
	for(int i=0; i<nRows(); i++)
	{	for(int j=0; j<nCols(); j++)
		{	complex& c = thisData[index(i,j)];
			fscanf(fp, fmt, &c.real(), &c.imag());
		}
	}
}
void matrix::scan_real(FILE* fp)
{	complex* thisData = this->data();
	for(int i=0; i<nRows(); i++)
	{	for(int j=0; j<nCols(); j++)
		{	complex& c = thisData[index(i,j)];
			fscanf(fp, "%lg", &c.real());
			c.imag() = 0;
		}
	}
}
void matrix::print(FILE* fp, const char* fmt) const
{	const complex* thisData = this->data();
	for(int i=0; i<nRows(); i++)
	{	for(int j=0; j<nCols(); j++)
		{	const complex& c = thisData[index(i,j)];
			fprintf(fp, fmt, c.real(), c.imag());
		}
		fprintf(fp,"\n");
	}
}
void matrix::print_real(FILE* fp, const char* fmt) const
{	const complex* thisData = this->data();
	for(int i=0; i<nRows(); i++)
	{	for(int j=0; j<nCols(); j++)
			fprintf(fp, fmt, thisData[index(i,j)].real());
		fprintf(fp,"\n");
	}
}


//------------------------- Eigensystem -----------------------------------

extern "C"
{	void zheevr_(char* JOBZ, char* RANGE, char* UPLO, int * N, complex* A, int * LDA,
		double* VL, double* VU, int* IL, int* IU, double* ABSTOL, int* M,
		double* W, complex* Z, int* LDZ, int* ISUPPZ, complex* WORK, int* LWORK,
		double* RWORK, int* LRWORK, int* IWORK, int* LIWORK, int* INFO);
}
void matrix::diagonalize(matrix& evecs, diagMatrix& eigs) const
{	static StopWatch watch("matrix::diagonalize");
	watch.start();
	
	myassert(nCols()==nRows());
	int N = nRows();
	myassert(N > 0);
	
	//Check hermiticity
	const complex* thisData = data();
	double errNum=0.0, errDen=0.0;
	for(int i=0; i<N; i++)
		for(int j=0; j<N; j++)
		{	errNum += norm(thisData[index(i,j)]-thisData[index(j,i)].conj());
			errDen += norm(thisData[index(i,j)]);
		}
	double hermErr = sqrt(errNum / (errDen*N));
	if(hermErr > 1e-10)
	{	logPrintf("Relative hermiticity error of %le (>1e-10) encountered in diagonalize\n", hermErr);
		stackTraceExit(1);
	}
	
	char jobz = 'V'; //compute eigenvectors and eigenvalues
	char range = 'A'; //compute all eigenvalues
	char uplo = 'U'; //use upper-triangular part
	matrix A = *this; //copy input matrix (zheevr destroys input matrix)
	double eigMin = 0., eigMax = 0.; //eigenvalue range (not used for range-type 'A')
	int indexMin = 0, indexMax = 0; //eignevalue index range (not used for range-type 'A')
	double absTol = 0.; int nEigsFound;
	eigs.resize(N);
	evecs.init(N, N);
	std::vector<int> iSuppz(2*N);
	int lwork = (64+1)*N; std::vector<complex> work(lwork); //Magic number 64 obtained by running ILAENV as suggested in doc of zheevr (and taking the max over all N)
	int lrwork = 24*N; std::vector<double> rwork(lrwork); //from doc of zheevr
	int liwork = 10*N; std::vector<int> iwork(liwork); //from doc of zheevr
	int info=0;
	zheevr_(&jobz, &range, &uplo, &N, A.data(), &N,
		&eigMin, &eigMax, &indexMin, &indexMax, &absTol, &nEigsFound,
		eigs.data(), evecs.data(), &N, iSuppz.data(), work.data(), &lwork,
		rwork.data(), &lrwork, iwork.data(), &liwork, &info);
	if(info<0) { logPrintf("Argument# %d to LAPACK eigenvalue routine ZHEEVR is invalid.\n", -info); stackTraceExit(1); }
	if(info>0) { logPrintf("Error code %d in LAPACK eigenvalue routine ZHEEVR.\n", info); stackTraceExit(1); }
	watch.stop();
}

extern "C"
{	void zgeev_(char* JOBVL, char* JOBVR, int* N, complex* A, int* LDA,
	complex* W, complex* VL, int* LDVL, complex* VR, int* LDVR,
	complex* WORK, int* LWORK, double* RWORK, int* INFO);
}
void matrix::diagonalize(matrix& levecs, std::vector<complex>& eigs, matrix& revecs) const
{	static StopWatch watch("matrix::diagonalizeNH");
	watch.start();
	//Prepare inputs and outputs:
	matrix A = *this; //destructible copy
	int N = A.nRows();
	myassert(N > 0);
	myassert(A.nCols()==N);
	eigs.resize(N);
	levecs.init(N, N);
	revecs.init(N, N);
	//Prepare temporaries:
	char jobz = 'V'; //compute eigenvectors and eigenvalues
	int lwork = (64+1)*N; std::vector<complex> work(lwork); //Magic number 64 obtained by running ILAENV as suggested in doc of zheevr (and taking the max over all N)
	std::vector<double> rwork(2*N);
	//Call LAPACK and check errors:
	int info=0;
	zgeev_(&jobz, &jobz, &N, A.data(), &N, eigs.data(), levecs.data(), &N, revecs.data(), &N, work.data(), &lwork, rwork.data(), &info);
	if(info<0) { logPrintf("Argument# %d to LAPACK eigenvalue routine ZGEEV is invalid.\n", -info); stackTraceExit(1); }
	if(info>0) { logPrintf("Error code %d in LAPACK eigenvalue routine ZGEEV.\n", info); stackTraceExit(1); }
	watch.stop();
}

extern "C"
{	void zgesdd_(char* JOBZ, int* M, int* N, complex* A, int* LDA,
		double* S, complex* U, int* LDU, complex* VT, int* LDVT,
		complex* WORK, int* LWORK, double* RWORK, int* IWORK, int* INFO);
	void zgesvd_(char* JOBU, char* JOBVT, int* M, int* N, complex* A, int* LDA,
		double* S, complex* U, int* LDU, complex* VT, int* LDVT,
		complex* WORK, int* LWORK, double* RWORK, int* INFO);
}
void matrix::svd(matrix& U, diagMatrix& S, matrix& Vdag) const
{	static StopWatch watch("matrix::svd");
	watch.start();
	//Initialize input and outputs:
	matrix A = *this; //destructible copy
	int M = A.nRows();
	int N = A.nCols();
	U.init(M,M);
	Vdag.init(N,N);
	S.resize(std::min(M,N));
	//Initialize temporaries:
	char jobz = 'A'; //full SVD (return complete unitary matrices)
	int lwork = 2*(M*N + M + N);
	std::vector<complex> work(lwork);
	std::vector<double> rwork(S.nRows() * std::max(5*S.nRows()+7, 2*(M+N)+1));
	std::vector<int> iwork(8*S.nRows());
	//Call LAPACK and check errors:
	int info=0;
	zgesdd_(&jobz, &M, &N, A.data(), &M, S.data(), U.data(), &M, Vdag.data(), &N,
		work.data(), &lwork, rwork.data(), iwork.data(), &info);
	if(info>0) //convergence failure; try the slower stabler version
	{	int info=0;
		matrix A = *this; //destructible copy
		zgesvd_(&jobz, &jobz, &M, &N, A.data(), &M, S.data(), U.data(), &M, Vdag.data(), &N,
			work.data(), &lwork, rwork.data(), &info);
		if(info<0) { logPrintf("Argument# %d to LAPACK SVD routine ZGESVD is invalid.\n", -info); stackTraceExit(1); }
		if(info>0) { logPrintf("Error code %d in LAPACK SVD routine ZGESVD.\n", info); stackTraceExit(1); }
	}
	if(info<0) { logPrintf("Argument# %d to LAPACK SVD routine ZGESDD is invalid.\n", -info); stackTraceExit(1); }
	watch.stop();
}


//Apply pending transpose / dagger operations:
matrixScaledTransOp::operator matrix() const
{	if(op==CblasNoTrans) return scale * mat;
	else
	{	const complex* matData = mat.data();
		matrix ret(mat.nCols(), mat.nRows()); complex* retData = ret.data();
		for(int i=0; i < mat.nCols(); i++)
			for(int j=0; j < mat.nRows(); j++)
				retData[ret.index(i,j)] = scale * conjOp(matData[mat.index(j,i)]);
		return ret;
	}
}

//Complex conjugate:
matrix conj(const scaled<matrix>& A)
{	matrix B = A.data;
	double* Bdata = (double*)B.dataPref();
	callPref(eblas_dscal)(B.nData(), A.scale, Bdata, 2); //real parts
	callPref(eblas_dscal)(B.nData(), -A.scale, Bdata+1, 2); //imag parts
	return B;
}

// Hermitian adjoint
matrixScaledTransOp dagger(const scaled<matrix> &A)
{	return matrixScaledTransOp(A.data, A.scale, CblasConjTrans);
}
matrix dagger_symmetrize(const scaled<matrix> &A)
{	return 0.5*(dagger(A) + A);
}
// Transpose
matrixScaledTransOp transpose(const scaled<matrix> &A)
{	return matrixScaledTransOp(A.data, A.scale, CblasTrans);
}
matrix transpose_symmetrize(const scaled<matrix>& A)
{	return 0.5*(transpose(A) + A);
}


//----------------------- Arithmetic ---------------------

matrix operator*(const matrixScaledTransOp &m1st, const matrixScaledTransOp &m2st)
{	myassert(m1st.nCols() == m2st.nRows());
	const matrix& m1 = m1st.mat;
	const matrix& m2 = m2st.mat;
	double scaleFac = m1st.scale * m2st.scale;
	matrix ret(m1st.nRows(), m2st.nCols(), isGpuEnabled());
	callPref(eblas_zgemm)(m1st.op, m2st.op, ret.nRows(), ret.nCols(), m1st.nCols(),
		scaleFac, m1.dataPref(), m1.nRows(), m2.dataPref(), m2.nRows(),
		0.0, ret.dataPref(), ret.nRows());
	return ret;
}

matrix operator*(const diagMatrix& d, const matrix& m)
{	myassert(d.nCols()==m.nRows());
	matrix ret(m); //copy input to output
	//transfer the diagonal matrix to the GPU if required:
	#ifdef GPU_ENABLED
	double* dData; cudaMalloc(&dData, d.nRows()*sizeof(double));
	cudaMemcpy(dData, &d[0], d.nRows()*sizeof(double), cudaMemcpyHostToDevice);
	gpuErrorCheck();
	#else
	const double* dData = &d[0];
	#endif
	//Elementwise-multiply each column by the scale factors (better contiguous access pattern than row-wise)
	for(int j=0; j<ret.nCols(); j++)
		callPref(eblas_zmuld)(ret.nRows(), dData, 1, ret.dataPref()+ret.index(0,j), 1);
	#ifdef GPU_ENABLED
	cudaFree(dData);
	#endif
	return ret;
}

matrix operator*(const matrix& m, const diagMatrix& d)
{	myassert(m.nCols()==d.nRows());
	matrix ret(m); //copy input to out
	//Scale each column:
	for(int j=0; j<ret.nCols(); j++)
		callPref(eblas_zdscal)(ret.nRows(), d[j], ret.dataPref()+ret.index(0,j), 1);
	return ret;
}

diagMatrix operator*(const diagMatrix& d1, const diagMatrix& d2)
{	myassert(d1.nCols()==d2.nRows());
	diagMatrix ret(d1);
	for(int i=0; i<ret.nRows(); i++) ret[i] *= d2[i]; //elementwise multiply
	return ret;
}

void axpy(double alpha, const diagMatrix& x, matrix& y)
{	myassert(x.nRows()==y.nRows());
	myassert(x.nCols()==y.nCols());
	complex* yData = y.data();
	for(int i=0; i<y.nRows(); i++) yData[y.index(i,i)] += alpha * x[i];
}

void axpy(double alpha, const diagMatrix& x, diagMatrix& y)
{	myassert(x.nRows()==y.nRows());
	for(int i=0; i<y.nRows(); i++) y[i] += alpha * x[i];
}

diagMatrix clone(const diagMatrix& x) { return x; }
matrix clone(const matrix& x) { return x; }

double dot(const diagMatrix& x, const diagMatrix& y)
{	myassert(x.size()==y.size());
	double ret = 0.;
	for(size_t i=0; i<x.size(); i++)
		ret += x[i]*y[i];
	return ret;
}
double dot(const matrix& x, const matrix& y) { return dotc(x,y).real(); }

void randomize(diagMatrix& x) { for(size_t i=0; i<x.size(); i++) x[i] = Random::normal(); }
void randomize(matrix& x)
{	complex* xData = x.data();
	for(size_t i=0; i<x.nData(); i++)
		xData[i] = Random::normalComplex();
}



//--------- Eigensystem, nonlinear matrix functions, and their gradients ----------

extern "C"
{	void zgetrf_(int* M, int* N, complex* A, int* LDA, int* IPIV, int* INFO);
	void zgetri_(int* N, complex* A, int* LDA, int* IPIV, complex* WORK, int* LWORK, int* INFO);
}

matrix inv(const matrix& A)
{	static StopWatch watch("inv(matrix)");
	watch.start();
	int N = A.nRows();
	myassert(N > 0);
	myassert(N == A.nCols());
	matrix invA(A); //destructible copy
	int ldA = A.nRows(); //leading dimension
	std::vector<int> iPivot(N); //pivot info
	int info; //error code in return
	//LU decomposition (in place):
	zgetrf_(&N, &N, invA.data(), &ldA, iPivot.data(), &info);
	if(info<0) { logPrintf("Argument# %d to LAPACK LU decomposition routine ZGETRF is invalid.\n", -info); stackTraceExit(1); }
	if(info>0) { logPrintf("LAPACK LU decomposition routine ZGETRF found input matrix to be singular at the %d'th step.\n", info); stackTraceExit(1); }
	//Compute inverse in place:
	int lWork = (64+1)*N;
	std::vector<complex> work(lWork);
	zgetri_(&N, invA.data(), &ldA, iPivot.data(), work.data(), &lWork, &info);
	if(info<0) { logPrintf("Argument# %d to LAPACK matrix inversion routine ZGETRI is invalid.\n", -info); stackTraceExit(1); }
	if(info>0) { logPrintf("LAPACK matrix inversion routine ZGETRI found input matrix to be singular at the %d'th step.\n", info); stackTraceExit(1); }
	watch.stop();
	return invA;
}

diagMatrix inv(const diagMatrix& A)
{	diagMatrix invA = A;
	for(double& x: invA) x = 1./x;
	return invA;
}

matrix LU(const matrix& A)
{	static StopWatch watch("LU(matrix)");
	watch.start();
	
	// Perform LU decomposition
	int N = A.nRows();
	myassert(N > 0);
	myassert(N == A.nCols());
	matrix LU(A); //destructible copy
	int ldA = A.nRows(); //leading dimension
	std::vector<int> iPivot(N); //pivot info
	int info; //error code in return
	//LU decomposition (in place):
	zgetrf_(&N, &N, LU.data(), &ldA, iPivot.data(), &info);
	if(info<0) { logPrintf("Argument# %d to LAPACK LU decomposition routine ZGETRF is invalid.\n", -info); stackTraceExit(1); }

	watch.stop();
	return LU;
}

complex det(const matrix& A)
{
	matrix decomposition = LU(A);
	int N = A.nRows();
	
	// Multiplies the diagonal entries to get the determinant up to a sign
	complex determinant(1., 0.);
	for(int i=0; i<N; i++)
		determinant *= decomposition(i,i);

	return determinant;

}

double det(const diagMatrix& A)
{	double determinant = 1.;
	for(int i=0; i<A.nCols(); i++)
		determinant *= A[i];
	return determinant;
}

//Common implementation for the matrix nonlinear functions:
#define MATRIX_FUNC(code) \
	myassert(A.nRows()==A.nCols()); \
	matrix evecs; diagMatrix eigs(A.nRows()); \
	A.diagonalize(evecs, eigs); \
	std::vector<complex> eigOut(A.nRows()); \
	\
	for(int i=0; i<A.nRows(); i++) \
	{ \
		code \
	} \
	\
	if(Aevecs) *Aevecs = evecs; \
	if(Aeigs) *Aeigs = eigs; \
	return evecs * matrix(eigOut) * dagger(evecs);

// Compute matrix A^exponent, and optionally the eigensystem of A (if non-null)
matrix pow(const matrix& A, double exponent, matrix* Aevecs, diagMatrix* Aeigs)
{	MATRIX_FUNC
	(	if(exponent<0. && eigs[i]<=0.0)
		{	logPrintf("Eigenvalue# %d is non-positive (%le) in pow (exponent %lg)\n", i, eigs[i], exponent);
			stackTraceExit(1);
		}
		else if(exponent>=0. && eigs[i]<0.0)
		{	logPrintf("WARNING: Eigenvalue# %d is negative (%le) in pow (exponent %lg); zeroing it out.\n", i, eigs[i], exponent);
			eigs[i] = 0.;
		}
		else eigOut[i] = pow(eigs[i], exponent);
	)
}

// Compute matrix A^-0.5 and optionally the eigensystem of A (if non-null)
matrix invsqrt(const matrix& A, matrix* Aevecs, diagMatrix* Aeigs)
{	return pow(A, -0.5, Aevecs, Aeigs);
}

// Compute cis(A) = exp(iota A) and optionally the eigensystem of A (if non-null)
matrix cis(const matrix& A, matrix* Aevecs, diagMatrix* Aeigs)
{	MATRIX_FUNC
	(	eigOut[i] = cis(eigs[i]);
	)
}

#undef MATRIX_FUNC

//Inverse of cis: get the Hermitian arg() of a unitary matrix
matrix cisInv(const matrix& A, matrix* Bevecs, diagMatrix* Beigs)
{	//Make sure A is unitary:
	myassert(A.nRows()==A.nCols());
	myassert(nrm2(A*dagger(A) - eye(A.nRows())) < 1e-10*sqrt(A.nData()));
	//Diagonalize:
	matrix Alevecs, Arevecs; std::vector<complex> Aeigs;
	A.diagonalize(Alevecs, Aeigs, Arevecs);
	myassert(nrm2(Alevecs-Arevecs) < 1e-10*sqrt(A.nData()));
	//Compute diagonal of result:
	diagMatrix B(A.nRows());
	for(int i=0; i<A.nRows(); i++)
		B[i] = Aeigs[i].arg();
	//Return results:
	if(Bevecs) *Bevecs = Arevecs;
	if(Beigs) *Beigs = B;
	matrix ret = Arevecs * B * dagger(Arevecs);
	return ret;
}



//Common implementation of the matrix nonlinear function gradients
#define MATRIX_FUNC_GRAD(code) \
	myassert(gradIn.nRows()==gradIn.nCols()); \
	myassert(Aevecs.nRows()==Aevecs.nCols()); \
	myassert(Aevecs.nRows()==gradIn.nCols()); \
	matrix AevecsDag = dagger(Aevecs); \
	\
	matrix gradOut = AevecsDag * gradIn * Aevecs; \
	complex* gradOutData = gradOut.data(); \
	for(int i=0; i<gradOut.nRows(); i++) \
		for(int j=0; j<gradOut.nCols(); j++) \
		{	complex& elem = gradOutData[gradOut.index(i,j)]; \
			code \
		} \
	return Aevecs * gradOut * AevecsDag;

// Return gradient w.r.t A given gradient w.r.t sqrt(A) and A's eigensystem
matrix sqrt_grad(const matrix& gradIn, const matrix& Aevecs, const diagMatrix& Aeigs)
{	MATRIX_FUNC_GRAD
	(	elem /= (sqrt(Aeigs[i])+sqrt(Aeigs[j]));
	)
}
// Return gradient w.r.t A given gradient w.r.t cis(A) and A's eigensystem
matrix cis_grad(const matrix& gradIn, const matrix& Aevecs, const diagMatrix& Aeigs)
{	MATRIX_FUNC_GRAD
	(	double x = Aeigs[j] - Aeigs[i];
		elem *= fabs(x)<1.0e-13 ? complex(-0.5*x,1) : (cis(x)-1.0)/x;
	)
}
#undef MATRIX_FUNC_GRAD

//---------------- Misc matrix ops --------------------

complex trace(const matrix &A)
{	myassert(A.nRows() == A.nCols());
	const complex* Adata = A.data();
	complex tr = 0.0;
	for(int i=0; i<A.nRows(); i++)
		tr += Adata[A.index(i,i)];
	return tr;
}

double trace(const diagMatrix& A)
{	double ret=0.0;
	for(double d: A) ret += d;
	return ret;
}

double nrm2(const diagMatrix& A)
{	double ret=0.0;
	for(double d: A) ret += d*d;
	return sqrt(ret);
}

diagMatrix diag(const matrix &A)
{	myassert(A.nRows()==A.nCols());
	diagMatrix ret(A.nRows());
	const complex* Adata = A.data();
	for(int i=0; i<A.nRows(); i++) ret[i] = Adata[A.index(i,i)].real();
	return ret;
}

diagMatrix eye(int N)
{	diagMatrix ret(N, 1.);
	return ret;
}

matrix zeroes(int nRows, int nCols)
{	matrix ret(nRows, nCols, isGpuEnabled());
	ret.zero();
	return ret;
}


//------------ Block matrices ------------

tiledBlockMatrix::tiledBlockMatrix(const matrix& mBlock, int nBlocks, const std::vector<complex>* phaseArr) : mBlock(mBlock), nBlocks(nBlocks), phaseArr(phaseArr)
{	if(phaseArr) myassert(nBlocks==int(phaseArr->size()));
}

matrix tiledBlockMatrix::operator*(const matrix& other) const
{	myassert(mBlock.nCols()*nBlocks == other.nRows());
	matrix result(mBlock.nRows()*nBlocks, other.nCols(), isGpuEnabled());
	//Dense matrix multiply for each block:
	for(int iBlock=0; iBlock<nBlocks; iBlock++)
	{	int offs = iBlock * mBlock.nCols();
		complex phase = phaseArr ? phaseArr->at(iBlock) : 1.;
		callPref(eblas_zgemm)(CblasNoTrans, CblasNoTrans, mBlock.nRows(), other.nCols(), mBlock.nCols(),
			phase, mBlock.dataPref(), mBlock.nRows(), other.dataPref()+offs, other.nRows(),
			0.0, result.dataPref()+offs, result.nRows());
	}
	return result;
}
