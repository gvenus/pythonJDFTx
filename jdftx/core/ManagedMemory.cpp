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

#include <core/ManagedMemory.h>
#include <core/BlasExtra.h>
#include <core/GpuUtil.h>
#include <fftw3.h>
#include <mutex>

//-------- Memory usage profiler ---------

namespace MemUsageReport
{
	enum Mode { Add, Remove, Print };
	
	//Add, remove or retrieve memory report based on mode
	void manager(Mode mode, string category=string(), size_t nElements=0)
	{	
		#ifdef ENABLE_PROFILING
		struct Usage
		{	size_t current, peak; //!< current and peak memory usage (in unit of complex numbers i.e. 16 bytes)
			Usage() : current(0), peak(0) {}
			
			Usage& operator+=(size_t n)
			{	current += n;
				if(current > peak)
					peak = current;
				return *this;
			}
			
			Usage& operator-=(size_t n)
			{	current -= n;
				return *this;
			}
		};
		static std::map<string, Usage> usageMap;
		static Usage usageTotal;
		static std::mutex usageLock;
		
		switch(mode)
		{	case Add:
			{	usageLock.lock();
				usageMap[category] += nElements;
				usageTotal += nElements;
				usageLock.unlock();
				myassert(category.length());
				break;
			}
			case Remove:
			{	usageLock.lock();
				usageMap[category] -= nElements;
				usageTotal -= nElements;
				usageLock.unlock();
				myassert(category.length());
				break;
			}
			case Print:
			{	const double elemToGB = 16./pow(1024.,3);
				for(auto entry: usageMap)
					logPrintf("MEMUSAGE: %30s %12.6lf GB\n", entry.first.c_str(), entry.second.peak * elemToGB);
				logPrintf("MEMUSAGE: %30s %12.6lf GB\n", "Total", usageTotal.peak * elemToGB);
				break;
			}
		}
		#endif //ENABLE_PROFILING
	}
}


//---------- class ManagedMemory -----------

// Construct, optionally with data allocation
ManagedMemory::ManagedMemory()
: nElements(0),c(0),onGpu(false)
{
}

ManagedMemory::~ManagedMemory()
{	memFree();
}

//Free memory
void ManagedMemory::memFree()
{	if(!nElements) return; //nothing to free
	if(onGpu)
	{
		#ifdef GPU_ENABLED
		myassert(isGpuMine());
		cudaFree(c);
		gpuErrorCheck();
		#else
		myassert(!"onGpu=true without GPU_ENABLED"); //Should never get here!
		#endif
	}
	else
	{	fftw_free(c);
	}
	MemUsageReport::manager(MemUsageReport::Remove, category, nElements);
	c = 0;
	nElements = 0;
	category.clear();
}

//Allocate memory
void ManagedMemory::memInit(string category, size_t nElements, bool onGpu)
{	if(category==this->category && nElements==this->nElements && onGpu==this->onGpu) return; //already in required state
	memFree();
	this->category = category;
	this->nElements = nElements;
	this->onGpu = onGpu;
	if(onGpu)
	{
		#ifdef GPU_ENABLED
		myassert(isGpuMine());
		cudaMalloc(&c, sizeof(complex)*nElements);
		gpuErrorCheck();
		#else
		myassert(!"onGpu=true without GPU_ENABLED");
		#endif
	}
	else
	{	c = (complex*)fftw_malloc(sizeof(complex)*nElements);
		if(!c) die_alone("Memory allocation failed (out of memory)\n");
	}
	MemUsageReport::manager(MemUsageReport::Add, category, nElements);
}

void ManagedMemory::memMove(ManagedMemory&& mOther)
{	std::swap(category, mOther.category);
	std::swap(nElements, mOther.nElements);
	std::swap(onGpu, mOther.onGpu);
	std::swap(c, mOther.c);
	//Now mOther will be empty, while *this will have all its contents
}

complex* ManagedMemory::data()
{
	#ifdef GPU_ENABLED
	toCpu();
	#endif
	return c;
}

const complex* ManagedMemory::data() const
{
	#ifdef GPU_ENABLED
	((ManagedMemory*)this)->toCpu(); //logically const, but may change data location
	#endif
	return c;
}



#ifdef GPU_ENABLED

complex* ManagedMemory::dataGpu()
{	toGpu();
	return (complex*)c;
}

const complex* ManagedMemory::dataGpu() const
{	((ManagedMemory*)this)->toGpu(); //logically const, but may change data location
	return (complex*)c;
}

//Move data to CPU
void ManagedMemory::toCpu()
{	if(!onGpu || !c) return; //already on cpu, or no data
	myassert(isGpuMine());
	complex* cCpu = (complex*)fftw_malloc(sizeof(complex)*nElements);
	if(!cCpu) die_alone("Memory allocation failed (out of memory)\n");
	cudaMemcpy(cCpu, c, sizeof(complex)*nElements, cudaMemcpyDeviceToHost); gpuErrorCheck();
	cudaFree(c); gpuErrorCheck(); //Free GPU mem
	c = cCpu; //Make c a cpu pointer
	onGpu = false;
}

// Move data to GPU
void ManagedMemory::toGpu()
{	if(onGpu || !c) return; //already on gpu, or no data
	myassert(isGpuMine());
	complex* cGpu;
	cudaMalloc(&cGpu, sizeof(complex)*nElements); gpuErrorCheck();
	cudaMemcpy(cGpu, c, sizeof(complex)*nElements, cudaMemcpyHostToDevice);
	fftw_free(c); //Free CPU mem
	c = cGpu; //Make c a gpu pointer
	onGpu = true;
}

#endif

void ManagedMemory::send(int dest, int tag) const
{	myassert(mpiUtil->nProcesses()>1);
	mpiUtil->send((const double*)data(), 2*nData(), dest, tag);
}
void ManagedMemory::recv(int src, int tag)
{	myassert(mpiUtil->nProcesses()>1);
	mpiUtil->recv((double*)data(), 2*nData(), src, tag);
}
void ManagedMemory::bcast(int root)
{	if(mpiUtil->nProcesses()>1)
		mpiUtil->bcast((double*)data(), 2*nData(), root);
}
void ManagedMemory::allReduce(MPIUtil::ReduceOp op, bool safeMode, bool ignoreComplexCheck)
{	if(!ignoreComplexCheck)
		myassert(op!=MPIUtil::ReduceProd && op!=MPIUtil::ReduceMax && op!=MPIUtil::ReduceMin); //not supported for complex
	if(mpiUtil->nProcesses()>1)
		mpiUtil->allReduce((double*)data(), 2*nData(), op, safeMode);
}


void ManagedMemory::write(const char *fname) const
{	FILE *fp = fopen(fname,"wb");
	if(!fp) die("Error opening %s for writing.\n", fname);
	write(fp);
	fclose(fp);
}
void ManagedMemory::writea(const char *fname) const
{	FILE *fp = fopen(fname,"ab");
	if(!fp) die("Error opening %s for appending.\n", fname);
	write(fp);
	fclose(fp);
}
void ManagedMemory::write(FILE *fp) const
{	size_t nDone = fwrite(data(), sizeof(complex), nData(), fp);
	if(nDone<nData()) die("Error after processing %lu of %lu records.\n", nDone, nData());
}
void ManagedMemory::dump(const char* fname, bool realPartOnly) const
{	logPrintf("Dumping '%s' ... ", fname); logFlush();
	if(realPartOnly)
	{	write_real(fname);
		//Collect imaginary part:
		double nrm2tot = nrm2(*this); 
		double nrm2im = callPref(eblas_dnrm2)(nData(), ((double*)dataPref())+1, 2); //look only at imaginary parts with a stride of 2
		logPrintf("done. Relative discarded imaginary part: %le\n", nrm2im / nrm2tot);
	}
	else
	{	write(fname);
		logPrintf("done.\n");
	}
}

void ManagedMemory::read(const char *fname)
{	off_t fsizeExpected = nData() * sizeof(complex);
	off_t fsize = fileSize(fname);
	if(fsize != off_t(fsizeExpected))
		die("Length of '%s' was %ld instead of the expected %ld bytes.\n", fname, fsize, fsizeExpected);
	FILE *fp = fopen(fname, "rb");
	if(!fp) die("Error opening %s for reading.\n", fname);
	read(fp);
	fclose(fp);
}
void ManagedMemory::read(FILE *fp)
{	size_t nDone = fread(data(), sizeof(complex), nData(), fp);
	if(nDone<nData()) die("Error after processing %lu of %lu records.\n", nDone, nData());
}


void ManagedMemory::write_real(const char *fname) const
{	FILE *fp = fopen(fname,"wb");
	if(!fp) die("Error opening %s for writing.\n", fname);
	write_real(fp);
	fclose(fp);
}
void ManagedMemory::write_real(FILE *fp) const
{	const complex* thisData = this->data();
	double *dataReal = new double[nData()];
	for(size_t i=0; i<nData(); i++) dataReal[i] = thisData[i].real();
	fwrite(dataReal, sizeof(double), nData(), fp);
	delete[] dataReal;
}

void ManagedMemory::read_real(const char *fname)
{	FILE *fp = fopen(fname,"rb");
	read_real(fp);
	fclose(fp);
}
void ManagedMemory::read_real(FILE *fp)
{	double *dataReal = new double[nData()];
	fread(dataReal, sizeof(double), nData(), fp);
	complex* thisData = this->data();
	for (size_t i=0; i<nData(); i++) thisData[i] = dataReal[i];
	delete[] dataReal;
}

void ManagedMemory::zero()
{	callPref(eblas_zero)(nData(), dataPref());
}

void ManagedMemory::reportUsage()
{	MemUsageReport::manager(MemUsageReport::Print);
}

void memcpy(ManagedMemory& a, const ManagedMemory& b)
{	myassert(a.nData() == b.nData());
	if(!a.nData()) return; //no data to copy
	#ifdef GPU_ENABLED
	cudaMemcpy(a.dataGpu(), b.dataGpu(), a.nData()*sizeof(complex), cudaMemcpyDeviceToDevice);
	#else
	memcpy(a.data(), b.data(), a.nData()*sizeof(complex));
	#endif
}

void scale(double alpha, ManagedMemory& y)
{	callPref(eblas_zdscal)(y.nData(), alpha, y.dataPref(), 1);
}
void scale(complex alpha, ManagedMemory& y)
{	callPref(eblas_zscal)(y.nData(), alpha, y.dataPref(), 1);
}

void axpy(complex alpha, const ManagedMemory& x, ManagedMemory& y)
{	myassert(x.nData() == y.nData());
	callPref(eblas_zaxpy)(x.nData(), alpha, x.dataPref(), 1, y.dataPref(), 1);
}

double nrm2(const ManagedMemory& a)
{	return callPref(eblas_dznrm2)(a.nData(), a.dataPref(), 1);
}

complex dotc(const ManagedMemory& a, const ManagedMemory& b)
{	myassert(a.nData() == b.nData());
	return callPref(eblas_zdotc)(a.nData(), a.dataPref(), 1, b.dataPref(), 1);
}
