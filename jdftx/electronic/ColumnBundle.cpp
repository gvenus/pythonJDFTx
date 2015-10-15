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

#include <electronic/Everything.h>
#include <electronic/ColumnBundle.h>
#include <electronic/matrix.h>
#include <electronic/operators.h>
#include <core/vector3.h>
#include <core/Random.h>
#include <core/BlasExtra.h>
#include <core/ScalarFieldIO.h>
#include <fftw3.h>

// Called by other constructors to do the work
void ColumnBundle::init(int nc, size_t len, const Basis *b, const QuantumNumber* q, bool onGpu)
{
	ncols = nc;
	col_length = len;
	basis = b;
	qnum = q;

	if(nCols() == 0) { memFree(); return; } //must be default constructor or assignment to empty ColumnBundle
	myassert(colLength() != 0);
	memInit("ColumnBundle", nCols()*colLength(), onGpu); //in base class ManagedMemory
}

void ColumnBundle::free()
{	ncols = 0;
	col_length = 0;
	basis = 0;
	qnum = 0;
	memFree();
}

// (Default) Constructor
ColumnBundle::ColumnBundle(int nc, size_t len, const Basis *b, const QuantumNumber* q, bool onGpu)
{	init(nc, len, b, q, onGpu);
}
// Copy constructor
ColumnBundle::ColumnBundle(const ColumnBundle &Y)
{	init(Y.nCols(), Y.colLength(), Y.basis, Y.qnum, Y.isOnGpu()); //initialize size and storage
	if(nData()) memcpy((ManagedMemory&)*this, (const ManagedMemory&)Y); //copy data
}
// Move constructor
ColumnBundle::ColumnBundle(ColumnBundle&& Y)
{	std::swap(ncols, Y.ncols);
	std::swap(col_length, Y.col_length);
	std::swap(qnum, Y.qnum);
	std::swap(basis, Y.basis);
	memMove((ManagedMemory&&)Y); //cannibalize Y's data
}
// Create a similar object, without copying data
ColumnBundle ColumnBundle::similar(int ncOverride) const
{	return ColumnBundle(ncOverride<0 ? ncols : ncOverride, col_length, basis, qnum, isOnGpu());
}


// Copy-assignment
ColumnBundle& ColumnBundle::operator=(const ColumnBundle &Y)
{	init(Y.nCols(), Y.colLength(), Y.basis, Y.qnum, Y.isOnGpu()); //initialize size and storage
	if(nData()) memcpy((ManagedMemory&)*this, (const ManagedMemory&)Y); //copy data
	return *this;
}
// Move-assignment
ColumnBundle& ColumnBundle::operator=(ColumnBundle&& Y)
{	std::swap(ncols, Y.ncols);
	std::swap(col_length, Y.col_length);
	std::swap(qnum, Y.qnum);
	std::swap(basis, Y.basis);
	memMove((ManagedMemory&&)Y); //cannibalize Y's data
	return *this;
}
ColumnBundle clone(const ColumnBundle& Y)
{	return Y;
}

void randomize(ColumnBundle& x)
{	x.randomize(0, x.nCols());
}

double dot(const ColumnBundle& x, const ColumnBundle& y)
{	complex result = dotc(x, y)*2.0;
	return result.real();
}

ColumnBundle ColumnBundle::getSub(int colStart, int colStop) const
{	myassert(colStart>=0);
	myassert(colStop<=nCols());
	int nColsSub = colStop - colStart;
	myassert(nColsSub>0);
	ColumnBundle ret = this->similar(nColsSub);
	callPref(eblas_copy)(ret.dataPref(), dataPref()+colStart*colLength(), nColsSub*colLength());
	return ret;
}

void ColumnBundle::setSub(int colStart, const ColumnBundle& Y)
{	myassert(colStart>=0);
	myassert(colStart<nCols());
	myassert(colLength()==Y.colLength());
	int nColsSub = std::min(Y.nCols(), nCols()-colStart);
	callPref(eblas_copy)(dataPref()+colStart*colLength(), Y.dataPref(), nColsSub*colLength());
}

#define CHECK_COLUMN_INDEX \
	myassert(i>=0 && i<nCols()); \
	myassert(s>=0 && s<spinorLength());

complexScalarFieldTilde ColumnBundle::getColumn(int i, int s) const
{	const GridInfo& gInfo = *(basis->gInfo);
	CHECK_COLUMN_INDEX
	complexScalarFieldTilde full; nullToZero(full, gInfo); //initialize a full G-space vector to zero
	//scatter from the i'th column to the full vector:
	callPref(eblas_scatter_zdaxpy)(basis->nbasis, 1., basis->indexPref, dataPref()+index(i,s*basis->nbasis), full->dataPref());
	return full;
}

void ColumnBundle::setColumn(int i, int s, const complexScalarFieldTilde& full)
{	//Zero the i'th column:
	callPref(eblas_zero)(basis->nbasis, dataPref()+index(i,s*basis->nbasis));
	//Gather-accumulate from the full vector into the i'th column
	accumColumn(i,s, full);
}

void ColumnBundle::accumColumn(int i, int s, const complexScalarFieldTilde& full)
{	myassert(full);
	CHECK_COLUMN_INDEX
	//Gather-accumulate from the full vector into the i'th column
	callPref(eblas_gather_zdaxpy)(basis->nbasis, 1., basis->indexPref, full->dataPref(), dataPref()+index(i,s*basis->nbasis));
}
#undef CHECK_COLUMN_INDEX


// Allocate an array of ColumnBundles
void init(std::vector<ColumnBundle>& Y, int nbundles, int ncols, const Basis* basis, const ElecInfo* eInfo)
{	Y.resize(nbundles);
	if(ncols && basis && eInfo)
	{	myassert(nbundles >= eInfo->qStop);
		for(int q=eInfo->qStart; q<eInfo->qStop; q++)
			Y[q].init(ncols, basis[q].nbasis * eInfo->spinorLength(), basis+q, &eInfo->qnums[q], isGpuEnabled());
	}
}


// Randomize with a high frequency cutoff of 0.75 hartrees
void ColumnBundle::randomize(int colStart, int colStop)
{	myassert(basis->nbasis==colLength() || 2*basis->nbasis==colLength());
	complex* thisData = data(); //currently only on cpu
	for(size_t j=0; j<colLength(); j++)
	{	size_t jBasis = (j < basis->nbasis) ? j : (j - basis->nbasis);
		vector3<> kplusG = basis->iGarr[jBasis] + qnum->k;
		double KE = 0.5*dot(kplusG, basis->gInfo->GGT*kplusG);
		double t = KE/0.75;
		double sigma = 1.0/(1.0+t*t*t*t*t*t);
		for(int i=colStart; i < colStop; i++)
			thisData[index(i,j)] = Random::normalComplex(sigma);
	}
}
void randomize(std::vector<ColumnBundle>& Y, const ElecInfo& eInfo)
{	for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		if(Y[q]) Y[q].randomize(0, Y[q].nCols());
}

//--------- Read/write an array of ColumnBundles from/to a file --------------

void write(const std::vector<ColumnBundle>& Y, const char* fname, const ElecInfo& eInfo)
{	//Compute output length from each process:
	std::vector<long> nBytes(mpiUtil->nProcesses(), 0); //total bytes to be written on each process
	for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		nBytes[mpiUtil->iProcess()] += Y[q].nData()*sizeof(complex);
	//Sync nBytes across processes:
	if(mpiUtil->nProcesses()>1)
		for(int iSrc=0; iSrc<mpiUtil->nProcesses(); iSrc++)
			mpiUtil->bcast(nBytes[iSrc], iSrc);
	//Compute offset of current process, and expected file length:
	long offset=0, fsize=0;
	for(int iSrc=0; iSrc<mpiUtil->nProcesses(); iSrc++)
	{	if(iSrc<mpiUtil->iProcess()) offset += nBytes[iSrc];
		fsize += nBytes[iSrc];
	}
	//Write to file:
	MPIUtil::File fp; mpiUtil->fopenWrite(fp, fname);
	mpiUtil->fseek(fp, offset, SEEK_SET);
	for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		mpiUtil->fwrite(Y[q].data(), sizeof(complex), Y[q].nData(), fp);
	mpiUtil->fclose(fp);
}


ColumnBundleReadConversion::ColumnBundleReadConversion()
: realSpace(false), nBandsOld(0), Ecut(0), EcutOld(0)
{
}

void read(std::vector<ColumnBundle>& Y, const char *fname, const ElecInfo& eInfo, const ColumnBundleReadConversion* conversion)
{	if(conversion && conversion->realSpace)
	{	if(eInfo.qStop==eInfo.qStart) return; //no k-point on this process
		const GridInfo* gInfoWfns = Y[eInfo.qStart].basis->gInfo;
		//Create a custom gInfo if necessary:
		GridInfo gInfoCustom;
		gInfoCustom.R = gInfoWfns->R;
		gInfoCustom.S = conversion->S_old;
		for(int k=0; k<3; k++) if(!gInfoCustom.S[k]) gInfoCustom.S[k] = gInfoWfns->S[k];
		bool needCustom = !(gInfoCustom.S == gInfoWfns->S);
		if(needCustom) { logSuspend(); gInfoCustom.initialize(); logResume(); }
		const GridInfo& gInfo = needCustom ? gInfoCustom : *gInfoWfns;
		//Read one column at a time:
		complexScalarField Icol; nullToZero(Icol, gInfo);
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		{	int nCols = Y[q].nCols();
			int nSpinor = Y[q].spinorLength();
			if(conversion->nBandsOld) nCols = std::min(nCols, conversion->nBandsOld);
			for(int b=0; b<nCols; b++) for(int s=0; s<nSpinor; s++)
			{	char fname_qb[1024]; sprintf(fname_qb, fname, q, b*nSpinor+s);
				loadRawBinary(Icol, fname_qb);
				if(needCustom) Y[q].setColumn(b,s, changeGrid(J(Icol), *gInfoWfns));
				else Y[q].setColumn(b,s, J(Icol));
			}
		}
	}
	else
	{	//Check if a conversion is actually needed:
		std::vector<ColumnBundle> Ytmp(eInfo.qStop);
		std::vector<Basis> basisTmp(eInfo.qStop);
		std::vector<long> nBytes(mpiUtil->nProcesses(), 0); //total bytes to be read on each process
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		{	bool needTmp = false, customBasis = false;
			int nCols = Y[q].nCols();
			if(conversion)
			{	if(conversion->nBandsOld && conversion->nBandsOld!=nCols)
				{	nCols = conversion->nBandsOld;
					needTmp = true;
				}
				double EcutOld = conversion->EcutOld ? conversion->EcutOld : conversion->Ecut;
				customBasis = (EcutOld!=conversion->Ecut);
				if(customBasis)
				{	needTmp = true;
					logSuspend();
					basisTmp[q].setup(*(Y[q].basis->gInfo), *(Y[q].basis->iInfo), EcutOld, Y[q].qnum->k);
					logResume();
				}
			}
			const Basis* basis = customBasis ? &basisTmp[q] : Y[q].basis;
			int nSpinor = Y[q].spinorLength();
			if(needTmp) Ytmp[q].init(nCols, basis->nbasis*nSpinor, basis, Y[q].qnum);
			nBytes[mpiUtil->iProcess()] += nCols * basis->nbasis*nSpinor * sizeof(complex);
		}
		//Sync nBytes:
		if(mpiUtil->nProcesses()>1)
			for(int iSrc=0; iSrc<mpiUtil->nProcesses(); iSrc++)
				mpiUtil->bcast(nBytes[iSrc], iSrc);
		//Compute offset of current process, and expected file length:
		long offset=0, fsize=0;
		for(int iSrc=0; iSrc<mpiUtil->nProcesses(); iSrc++)
		{	if(iSrc<mpiUtil->iProcess()) offset += nBytes[iSrc];
			fsize += nBytes[iSrc];
		}
		//Read data into Ytmp or Y as appropriate, and convert if necessary:
		MPIUtil::File fp; mpiUtil->fopenRead(fp, fname, fsize, "Hint: Did you specify the correct nBandsOld, EcutOld and kdepOld?\n");
		mpiUtil->fseek(fp, offset, SEEK_SET);
		for(int q=eInfo.qStart; q<eInfo.qStop; q++)
		{	ColumnBundle& Ycur = Ytmp[q] ? Ytmp[q] : Y[q];
			mpiUtil->fread(Ycur.data(), sizeof(complex), Ycur.nData(), fp);
			if(Ytmp[q]) //apply conversions:
			{	if(Ytmp[q].basis!=Y[q].basis)
				{	int nSpinor = Y[q].spinorLength();
					for(int b=0; b<std::min(Y[q].nCols(), Ytmp[q].nCols()); b++)
						for(int s=0; s<nSpinor; s++)
							Y[q].setColumn(b,s, Ytmp[q].getColumn(b,s)); //convert using the full G-space as an intermediate
				}
				else
				{	if(Ytmp[q].nCols()<Y[q].nCols()) Y[q].setSub(0, Ytmp[q]);
					else Y[q] = Ytmp[q].getSub(0, Y[q].nCols());
				}
				Ytmp[q].free();
			}
		}
		mpiUtil->fclose(fp);
	}
}

//------------------------ Arithmetic --------------------

ColumnBundle& operator+=(ColumnBundle& Y, const ColumnBundle &X) { if(Y) axpy(1.0, X, Y); else Y=X; return Y; }
ColumnBundle& operator-=(ColumnBundle& Y, const ColumnBundle &X) { if(Y) axpy(-1.0, X, Y); else Y=-X; return Y; }
ColumnBundle operator+(const ColumnBundle &Y1, const ColumnBundle &Y2) { ColumnBundle Ysum(Y1); Ysum += Y2; return Ysum; }
ColumnBundle operator-(const ColumnBundle &Y1,const ColumnBundle &Y2) { ColumnBundle Ydiff(Y1); Ydiff -= Y2; return Ydiff; }

ColumnBundle& operator*=(ColumnBundle& X, double s) { scale(s, X); return X; }
scaled<ColumnBundle> operator*(double s, const ColumnBundle &Y) { return scaled<ColumnBundle>(Y, s); }
scaled<ColumnBundle> operator*(const ColumnBundle &Y, double s) { return scaled<ColumnBundle>(Y, s); }
scaled<ColumnBundle> operator-(const ColumnBundle &Y) { return scaled<ColumnBundle>(Y, -1); }
ColumnBundle& operator*=(ColumnBundle& X, complex s) { scale(s, X); return X; }
ColumnBundle operator*(complex s, const ColumnBundle &Y) { ColumnBundle sY(Y); sY *= s; return sY; }
ColumnBundle operator*(const ColumnBundle &Y, complex s) { ColumnBundle sY(Y); sY *= s; return sY; }

ColumnBundle operator*(const scaled<ColumnBundle> &sY, const matrixScaledTransOp &Mst)
{	static StopWatch watch("Y*M");
	watch.start();
	const ColumnBundle& Y = sY.data;
	double scaleFac = sY.scale * Mst.scale;
	bool spinorMode = (2*Y.nCols() == Mst.nRows()); //treat each column of non-spinor Y as two identical consecutive spinor ones with opposite spins
	myassert(Y.nCols()==Mst.nRows() || spinorMode);
	CBLAS_TRANSPOSE Mop; const matrix* M; ColumnBundle YM; matrix Mtmp;
	if(spinorMode)
	{	matrix mIn(Mst); Mop=CblasNoTrans; //pre-apply the op in this case
		Mtmp.init(Y.nCols(), 2*mIn.nCols(), isGpuEnabled());
		Mtmp.set(0,1,Y.nCols(), 0,2,Mtmp.nCols(), mIn(0,2,mIn.nRows(), 0,1,mIn.nCols()));
		Mtmp.set(0,1,Y.nCols(), 1,2,Mtmp.nCols(), mIn(1,2,mIn.nRows(), 0,1,mIn.nCols()));
		M = &Mtmp;
		myassert(!Y.isSpinor());
		YM.init(mIn.nCols(), Y.colLength()*2, Y.basis, Y.qnum, isGpuEnabled());
	}
	else
	{	Mop = Mst.op;
		M = &Mst.mat;
		YM = Y.similar(Mst.nCols());
	}
	callPref(eblas_zgemm)(CblasNoTrans, Mop, Y.colLength(), M->nCols(), Y.nCols(),
		scaleFac, Y.dataPref(), Y.colLength(), M->dataPref(), M->nRows(),
		0.0, YM.dataPref(), Y.colLength());
	watch.stop();
	return YM;
}

ColumnBundle operator*(const scaled<ColumnBundle> &sY, const diagMatrix& d)
{	const ColumnBundle& Y = sY.data;
	myassert(Y.nCols()==d.nRows());
	ColumnBundle Yd = Y; complex* YdData = Yd.dataPref();
	for(int i=0; i<d.nCols(); i++)
		callPref(eblas_zscal)(Yd.colLength(), sY.scale*d[i], YdData+Yd.index(i,0), 1);
	return Yd;
}

matrix operator^(const scaled<ColumnBundle> &sY1, const scaled<ColumnBundle> &sY2)
{	static StopWatch watch("Y1^Y2");
	watch.start();
	const ColumnBundle& Y1 = sY1.data;
	const ColumnBundle& Y2 = sY2.data;
	double scaleFac = sY1.scale * sY2.scale;
	int nCols1, nCols2, colLength;
	if(Y1.colLength() == Y2.colLength()) //standard mode
	{	nCols1 = Y1.nCols();
		nCols2 = Y2.nCols();
		colLength = Y1.colLength();
	}
	else //exactly one of the two columnbundles is a spinor (but they have a common basis)
	{	myassert(Y1.basis);
		myassert(Y2.basis);
		myassert(Y1.basis->nbasis == Y2.basis->nbasis);
		myassert(Y1.isSpinor() xor Y2.isSpinor());
		nCols1 = Y1.nCols() * Y1.spinorLength();
		nCols2 = Y2.nCols() * Y2.spinorLength();
		colLength = Y1.basis->nbasis;
	}
	matrix Y1dY2(nCols1, nCols2, isGpuEnabled());
	callPref(eblas_zgemm)(CblasConjTrans, CblasNoTrans, nCols1, nCols2, colLength,
		scaleFac, Y1.dataPref(), colLength, Y2.dataPref(), colLength,
		0.0, Y1dY2.dataPref(), Y1dY2.nRows());
	watch.stop();
	//If one of the columnbundles was spinor, shape the matrix as if the non-spinor columnbundle had consecutive spinor columns with identical pure up and down spinors
	if(Y1.nCols() != nCols1) //Y1 is spinor, so double the dimension of output along Y2
	{	matrix out(Y1.nCols(), 2*nCols2);
		out.set(0,1,Y1.nCols(), 0,2,2*nCols2, Y1dY2(0,2,nCols1, 0,1,nCols2));
		out.set(0,1,Y1.nCols(), 1,2,2*nCols2, Y1dY2(1,2,nCols1, 0,1,nCols2));
		return out;
	}
	else if(Y2.nCols() != nCols2) //Y2 is spinor, so double the dimension of output along Y1
	{	matrix out(2*nCols1, Y2.nCols());
		out.set(0,2,2*nCols1, 0,1,Y2.nCols(), Y1dY2(0,1,nCols1, 0,2,nCols2));
		out.set(1,2,2*nCols1, 0,1,Y2.nCols(), Y1dY2(0,1,nCols1, 1,2,nCols2));
		return out;
	}
	else return Y1dY2; //normal mode (neither is a spinor)
}
