#include <windows.h>

#include <fstream>
#include <cstring>
#include <cmath>
#include <ctime>

#include <io.h>
#include <process.h>

#include "dicom.h"
#include "fft.h"

typedef float FP_VAR;	// complile with either single or double precision

enum filter_type {ramlak, shepplogan, hamming, hanning, cosine, blackman};

class Projection
{
public:
	Projection(const char* newDir);
	~Projection();	

	float getYOffset();		// returns the y-offset for the specified projection angle
	float getZOffset();

	int LoadNextProj();	// loads the next projection
	int Filter();
	int Interpolate(int** interp_map);

	void Subtract(FP_VAR** pd2, FP_VAR ratio);

	void CreateFilter(filter_type filter, double cutoff = 1.0);

	unsigned short GetNumProj() { return num_proj; }
	void CloseFindFile();

	void WriteBin(char* filename);

	friend class Reconstruction;

private:
	// directory
	char dir[1024];				// location of source files

	// projection parameters
	unsigned short num_proj;	// number of projections
	unsigned short rows;		// rows
	unsigned short cols;		// columns
	double detectorRes;			// resolution

	// calibration and geometry
	double sourceToDetector;
	double sourceToAxis;
	float YOffset[360];
	float ZOffset[360];

	// misc scan data
	int kVp;					// x-ray voltage
	double pitch;				// mm per revolution (not used yet)

	FP_VAR **blank;				// blank projection

	// current projection in memory
	unsigned short *dataBuffer;	// buffer for loading dicom data
	FP_VAR **pd;		// projection data
	double projAngle;	// angle of the current projection

	// projection filters
	FP_VAR *G;			// convolution function
	FP_VAR **cos_theta; // cos(theta) scaling
	FP_VAR *temp;		// temp buffer used for FFT transforms

	// file io handle
	intptr_t ff;			// Windows-specific file io using _findfirst and _findnext
};

Projection::Projection(const char* newDir)
{
	_finddata_t data;
	
	int len;
	char filename[MAX_PATH];

	RootDicomObj* DCMObj;

	int i,j;
	double y,z;
	char buffer[16];

	char temp_str[64];

	
	FP_VAR offset = 0.0f;
	FP_VAR slope = 0.0f;

	memcpy(dir,newDir, strlen(newDir)+1);

	// get the first file
	sprintf_s(filename,MAX_PATH,"%s\\1.3.6.1.4.1*",dir);
	if((ff = _findfirst(filename,&data)) == -1)
	{
		cout << "An error has occurred: " << errno;	// an error has occurred
	}

	// open the first file in the directory
	sprintf_s(filename,MAX_PATH,"%s\\%s",dir,data.name);
	DCMObj = new RootDicomObj(filename);

	// fill in rows, cols, num_proj, etc...
	DCMObj->GetValue(0x0028,0x0010,&rows,sizeof(rows));
	DCMObj->GetValue(0x0028,0x0011,&cols,sizeof(cols));
	DCMObj->GetValue(0x0054,0x0053,&num_proj,sizeof(num_proj));

	DCMObj->GetValue(0x0018,0x9306,&detectorRes,sizeof(detectorRes));
	DCMObj->GetValue(0x0018,0x9310,&pitch,sizeof(pitch));
	DCMObj->GetValue(0x0009,0x1046,ZOffset,sizeof(ZOffset));
	DCMObj->GetValue(0x0009,0x1047,YOffset,sizeof(YOffset));

	len = DCMObj->GetValue(0x0018, 0x1110, buffer, sizeof(buffer)); // source to detector (DS)
	buffer[len] = 0;
	sourceToDetector = atof(buffer);
	len = DCMObj->GetValue(0x0018, 0x1111, buffer, sizeof(buffer)); // source to axis (DS)
	buffer[len] = 0;
	sourceToAxis = atof(buffer);

	len = DCMObj->GetValue(0x0018, 0x0060, buffer, sizeof(buffer)); // source to axis (DS)
	buffer[len] = 0;
	kVp = atoi(buffer);

	// allocate memory for scan and blank
	pd = new FP_VAR*[rows];			// current working projection
	blank = new FP_VAR*[rows];		// 

	cos_theta = new FP_VAR*[rows];
	G = new FP_VAR[2*rows];			// double the length to facilitate zero padding
	temp = new FP_VAR[4*rows];

	for(i=0;i<rows;i++)
	{
		pd[i] = new FP_VAR[cols];
		blank[i] = new FP_VAR[cols];
		cos_theta[i] = new FP_VAR[cols];
	}

	dataBuffer = new unsigned short[rows*cols];

	// find the blank scan
	cout << "Loading blank scan" << endl;
	while(1)
	{
		// open the first file in the directory
		sprintf_s(filename,MAX_PATH,"%s\\%s",dir,data.name);
		DCMObj = new RootDicomObj(filename, true); //load header only

		// check ImageType
		DCMObj->GetValue(0x0008,0x0008,temp_str,sizeof(temp_str));
		if(strstr(temp_str,"BLANK SCAN"))
		{
			// load the blank scan data
			delete DCMObj;
			DCMObj = new RootDicomObj(filename); // reload with the data

			DCMObj->GetValue(0x7FE0,0x0010,(char*)dataBuffer, rows*cols*sizeof(dataBuffer));
			for(i=0;i<rows;i++)
			{
				for(j=0; j<cols; j++)
					blank[i][j] = dataBuffer[i*cols + j]; // + offset + slope*j;
			}

			break;
		}
		else if(_findnext(ff, &data) == -1L)
		{
			cout << "Warning: blank scan not found." << endl;
			for(i=0;i<rows;i++)
				for(j=0;j<cols;j++)
					blank[i][j] = 0.0;
			break;
		}

		delete DCMObj;
	}

	_findclose(ff);
	ff = -1;

	// create cos_theta scaling map
	for(i=0;i<rows;i++)
	{
		y = detectorRes * (i - (rows-1.0)/2.0) * (sourceToAxis/sourceToDetector);
		for(j=0;j<cols;j++)
		{
			z = detectorRes * (i - (cols-1.0)/2.0) * (sourceToAxis/sourceToDetector);
			cos_theta[i][j] = float(sourceToAxis / sqrt(sourceToAxis * sourceToAxis + y * y + z * z));
		}
	}

	// initialize other filter to unity (no filtering)
	for(i=0;i<(2*rows);i++)
		G[i] = 1.0;
}

Projection::~Projection()
{
	for(int i=0;i<rows;i++)
	{
		delete [] pd[i];
		delete [] blank[i];
		delete [] cos_theta[i];
	}

	delete [] pd;
	delete [] blank;
	delete [] cos_theta;
	delete [] G;
	delete [] temp;

	delete [] dataBuffer;
}

float Projection::getYOffset()
{
	int n;
	n = floor(projAngle+0.5);
	n += 180;			// projection angle is 180° from tube angle
	n = n % 360;
	return YOffset[n];
}

float Projection::getZOffset()
{
	int n;
	n = floor(projAngle+0.5);
	n += 180;			// projection angle is 180° from tube angle
	n = n % 360;
	return ZOffset[n];
}

void Projection::CreateFilter(filter_type filter, double cutoff)
{
	int i;

	double* w;

	w = new double[rows+1];

	for(i=0; i<=rows; i++)
	{
		G[i] = FP_VAR(i) / rows;
		w[i] = M_PI * double(i) /rows;
	}
	for(i=rows*cutoff+1;i<=rows;i++)
		G[i] = 0;

	switch(filter)
	{
	case ramlak:
		cout << "Ram-Lak filter, " << cutoff << " cutoff." << endl;
		break;

	case shepplogan:
		cout << "Shepp-Logan filter, " << cutoff << " cutoff." << endl;
		for(i=1; i<=rows; i++)
			G[i] *= sin(w[i]/(2*cutoff))/(w[i]/(2*cutoff));
		break;

	case hamming:
		cout << "Hamming filter, " << cutoff << " cutoff." << endl;
		for(i=1; i<=rows; i++)
			G[i] *= 0.54 + 0.46 * cos(w[i]/cutoff);
		break;

	case hanning:
		cout << "Hann filter, " << cutoff << " cutoff." << endl;
		for(i=1; i<=rows; i++)
			G[i] *= (1 + cos(w[i]/cutoff))/2;
		break;

	case cosine:
		cout << "Cosine filter, " << cutoff << " cutoff." << endl;
		for(i=1; i<=rows; i++)
			G[i] *= cos(w[i]/(2*cutoff));
		break;

	default:
		cout << "Unknown filter!" << endl;
		for(i=0; i<=rows; i++)
			G[i] = 1.0;
		break;
	}

	// mirror the filter
	for(;i<(2*rows);i++)
	{
		G[i] = G[2*rows-i];
	}

	delete [] w;
}

int Projection::LoadNextProj()
{
	_finddata_t data;

	int i,j;
	double P;
	RootDicomObj* DCMObj;

	bool done = false;

	char temp[64];
	char filespec[MAX_PATH];
	char filename[MAX_PATH];

	while(1)
	{
		if(ff == -1)	// load the first projection
		{
			sprintf_s(filespec,MAX_PATH,"%s\\1.3.6.1.4.1*",dir);
			ff = _findfirst(filespec, &data);
			if(ff == -1)	// no files found matching description
				return 0;
		}
		else
		{
			if(_findnext(ff, &data) == -1)	// _findnext will return 0 if sucessful
			{
				_findclose(ff);			// close the file handle
				ff = -1;
				return 0;				// no next file found
			}
		}

		sprintf_s(filename,MAX_PATH,"%s\\%s",dir,data.name);	
		DCMObj = new RootDicomObj(filename);

		DCMObj->GetValue(0x0008,0x0008,temp,sizeof(temp));
		if(strstr(temp,"BLANK SCAN"))
			delete DCMObj;
		else
			break;
	}

	FP_VAR offset = 0.0f;
	FP_VAR slope = 0.0f;

	DCMObj->GetValue(0x7FE0,0x0010,(char*)dataBuffer, rows*cols*sizeof(dataBuffer));
	for(i=0;i<rows;i++)
		for(j=0; j<cols; j++)
		{
				
			P = log(blank[i][j]/(FP_VAR(dataBuffer[i*cols + j]))); // + offset+ slope*j)));

			// empirical beam hardening correction
			
			switch(kVp)
			{
			case 45:	// empirical beam hardening for 45kVp
				P = 0.8346*P + 0.1656*P*P + 0.0069*P*P*P;
				break;
			case 55:
				P = 0.8260*P + 0.2111*P*P - 0.0042*P*P*P;
				break;
			case 65:
				P = 0.8159*P + 0.2636*P*P - 0.0195*P*P*P;
				break;
			default:	// no correction
				break;
			}
			
			pd[i][j] = P;
		}


	DCMObj->GetValue(0x0009,0x1036,(char*)&projAngle, sizeof(projAngle));

	delete DCMObj;

	return 1;
}

void Projection::CloseFindFile()
{
	if(ff==-1)
		return;
	_findclose(ff);
	ff = -1;
	return;
}

int Projection::Filter()
{
	int i,j;

	// cos(theta) scaling
	for(i=0; i<rows; i++)
		for(j=0; j<cols; j++)
			pd[i][j] *= cos_theta[i][j];

	// convolve projection with filter
	for(j=0;j<cols;j++)	// for each column
	{
		for(i=0;i<rows;i++)
		{
			temp[2*i] = pd[i][j];
			temp[2*i+1] = 0;
		}
		for(;i<(2*rows);i++)
		{
			temp[2*i] = 0;
			temp[2*i+1] = 0;
		}
		fft(temp,2*rows,1);
		for(i=0;i<(2*rows);i++)
		{
			temp[2*i] *= G[i];
			temp[2*i+1] *= G[i];
		}
		fft(temp,2*rows,-1);
		for(i=0;i<rows;i++)
			pd[i][j] = temp[2*i];
	}
	
	return 0;
}

int Projection::Interpolate(int** interp_map)
{
	int i,j;

	int int_start, int_end;

	int n;
	float delta;
	
	for (j=0;j<cols;j++)
	{
		for(i=0;i<rows;i++)
		{
			if(interp_map[i][j])
			{
				int_start = i-1;
				while(interp_map[i][j])
					i++;
				int_end = i-1;
				delta = (pd[int_end+1][j] - pd[int_start - 1][j])/(int_end - int_start + 2);
				for(n=int_start;n<=int_end;n++)
					pd[n][j] = pd[int_start-1][j] + (n - int_start + 1) * delta;

				i = int_end+1;
			}
		}

	}

	return 0;
}

void Projection::Subtract(FP_VAR **pd2, FP_VAR ratio)
{
	int i,j;
	for(i=0;i<rows;i++)
		for(j=0;j<cols;j++)
			pd[i][j] -= ratio * pd2[i][j];
}

void Projection::WriteBin(char* filename)
{
	int i;
	ofstream fout;

	fout.open(filename,ios::binary);
	for(i=0;i<rows;i++)
		fout.write(reinterpret_cast<char*>(pd[i]),cols*sizeof(FP_VAR));
	fout.close();
}

class Reconstruction
{
public:
	Reconstruction(int newSlices, int newRows, int newCols, double newRes, Projection* newProj, char* filename = NULL);	// loads a reconstruction from a file
	~Reconstruction();

	void Backproject();
	void RemoveMetal();		// 
	void SetMetalThreshold(double new_thresh) { threshold = new_thresh; }
	
	int WriteDicom(char* out_file);
	void WriteBin(char* out_file);

	void CancelRecon() { cancel = true; }
	void SetHWND(HWND hwnd) {hApp = hwnd;}
	HBITMAP GetBitmap();

	static unsigned __stdcall ReconThread(void* thread_param)
	{
		Reconstruction* pThis = (Reconstruction*)thread_param;
		pThis->cancel = false;
		pThis->Backproject();
		_endthreadex(0);

		return 0;	// never reached...
	}

	static unsigned __stdcall RemoveMetalThread(void* thread_param)
	{
		Reconstruction* pThis = (Reconstruction*)thread_param;
		pThis->cancel = false;
		pThis->RemoveMetal();
		SendMessage(pThis->hApp,WM_RECON_COMPLETE,NULL,NULL);
		_endthreadex(0);

		return 0;	// never reached...
	}

private:
	Projection *proj;
	FP_VAR*** recon;

	FP_VAR** display_slice;

	double res;
	int slices;
	int rows;
	int cols;

	double* x;
	double* y;
	double* z;

	FP_VAR threshold;

	bool cancel;
	HWND hApp;
	HANDLE hMutex;
};

Reconstruction::Reconstruction(int newSlices, int newRows, int newCols, double newRes, Projection* newProj, char* filename)
:slices(newSlices),rows(newRows), cols(newCols), res(newRes), proj(newProj)
{
	int i,j;
	ifstream f;

	cancel = false;
	hMutex = CreateMutex(NULL, FALSE, NULL);
	threshold = 10.0;

	// allocate memory
	recon = new FP_VAR**[slices];
	for(i=0;i<slices;i++)
		recon[i] = new FP_VAR*[rows];
	for(i=0;i<slices;i++)
		for(j=0;j<rows;j++)
		{
			recon[i][j] = new FP_VAR[cols];
			memset(recon[i][j],0,cols*sizeof(FP_VAR));	// initialize to zero...
		}

	display_slice = new FP_VAR*[rows];
	for(i=0;i<rows;i++)
		display_slice[i] = new FP_VAR[cols];

	z = new double[slices];
	for(i=0;i<slices;i++)
		z[i] = res * (i - (slices-1.0)/2);

	y = new double[rows];
	for(i=0;i<rows;i++)
		y[i] = res * (i - (rows-1.0)/2);

	x = new double[cols];
	for(i=0;i<cols;i++)
		x[i] = res * (i - (cols-1.0)/2);

	if(filename)
	{
		f.open(filename,ios::binary);

		for(i=0;i<slices;i++)
			for(j=0;j<rows;j++)
				f.read(reinterpret_cast<char*>(recon[i][j]),cols*sizeof(FP_VAR));
		f.close();
	}
}

void Reconstruction::Backproject()
{
	int i,j,k;
	double cos_theta, sin_theta;
	double YOffset, ZOffset;
	double x_r, y_r;		// rotated x,y coordinates
	double y_p, z_p;		// projected y,z coordinates
	int fy, fz;
	double dy, dz;
	double scale;

	unsigned short n=0;

	DWORD dwWaitResult;

	for(i=0;i<slices;i++)
		for(j=0;j<rows;j++)
			memset(recon[i][j],0,cols*sizeof(FP_VAR));	// initialize memory to zero...

	proj->LoadNextProj();	// get rid of intial 270???

	while(proj->LoadNextProj())
	{
		n++;

		cout << proj->projAngle << "ø" << endl;
		// proj->Interpolate(0.6);
		proj->Filter();

		cos_theta = cos(M_PI*(proj->projAngle + 90)/180);
		sin_theta = sin(M_PI*(proj->projAngle + 90)/180);
		YOffset = proj->getYOffset();
		ZOffset = proj->getZOffset();

		for(j=0;j<rows;j++)
		{
			for(k=0;k<cols;k++)
			{
				x_r = x[k] * cos_theta + y[j] * sin_theta;
				y_r = -x[k] * sin_theta + y[j] * cos_theta;
				y_p = y_r * (proj->sourceToDetector/(proj->sourceToAxis + x_r)) + YOffset;		// in mm
				y_p = ((proj->rows-1.0)/2.0) - (y_p/proj->detectorRes);

				scale = proj->sourceToAxis /(proj->sourceToAxis - x_r);
				scale *= scale;

				for(i=0;i<slices;i++)
				{
					
					z_p = z[i] * (proj->sourceToDetector/(proj->sourceToAxis + x_r)) + ZOffset;	// in mm
					z_p = (z_p/proj->detectorRes) + ((proj->cols-1.0)/2.0);
						
					fy = floor(y_p);
					fz = floor(z_p);

					dy = y_p - fy;
					dz = z_p - fz;

					if( (fy>0) && (fy < ( proj->rows - 1)) && (fz>0) && (fz<(proj->cols - 1)) )
						recon[i][j][k] += scale * 
									  (proj->pd[fy][fz] * (1-dy) * (1-dz) +	// bilinear interpolation
									  proj->pd[fy+1][fz] * dy * (1-dz) +
									  proj->pd[fy][fz+1] * (1-dy) * dz +
									  proj->pd[fy+1][fz+1] * dy * dz);

				}
				// check for cancel after each iteration
				if(cancel)
				{
					proj->CloseFindFile();
					// should reset progress bar
					return;
				}
			}
		}
		// copy current recon into display_slice
		dwWaitResult = WaitForSingleObject(hMutex,1000);
		if(dwWaitResult == WAIT_OBJECT_0)
		{
			for(j=0;j<rows;j++)
				for(k=0;k<cols;k++)
					display_slice[j][k] = recon[slices/2][j][k];
			ReleaseMutex(hMutex);
		}
		PostMessage(hApp,WM_UPDATE_RECON,MAKEWPARAM(n,proj->num_proj),NULL);
	}
	// annouce that reconstruction is finished and reset progress bar
	PostMessage(hApp,WM_RECON_COMPLETE,NULL,NULL);

}

Reconstruction::~Reconstruction()
{
	int i,j;

	for(i=0;i<slices;i++)
		for(j=0;j<rows;j++)
			delete [] recon[i][j];
	for(i=0;i<slices;i++)
		delete [] recon[i];
	delete [] recon;

	delete [] x;
	delete [] y;
	delete [] z;

}


int Reconstruction::WriteDicom(char* out_file)
{
	int i,j,k;
	
	time_t _Time;
	struct tm* timeinfo = new tm;

	DataElement* DE;
	RootDicomObj *DCMObj = new RootDicomObj();
	RootDicomObj *proj_dcm;
	char SOPInstanceUID[256];
	char SeriesInstanceUID[256];
	char temp[256];
	char* p_ch;
	unsigned short* p_us;
	unsigned long len;
	unsigned short us_temp;


	ofstream f;

	intptr_t ff;			// Windows-specific file io using _findfirst and _findnext
	_finddata_t data;
	char filename[MAX_PATH];

	// get a Dicom Structure with the scan data
	sprintf_s(filename,MAX_PATH,"%s\\1.3.6.1.4.1*",proj->dir);
	if((ff = _findfirst(filename,&data)) == -1)
	{
		cout << "An error has occurred: " << errno;	// an error has occurred
	}
	_findclose(ff);

	// open the first file in the directory
	sprintf_s(filename,MAX_PATH,"%s\\%s",proj->dir,data.name);
	proj_dcm = new RootDicomObj(filename);

	time(&_Time);
	srand((unsigned int)_Time);
	localtime_s(timeinfo, &_Time);

	memset(SOPInstanceUID,0,sizeof(SOPInstanceUID));
	sprintf_s(SOPInstanceUID,256,"1.2.276.0.7230010.3.1.4.342487148.%d.%d%d%d.%d",rand()%10000, rand()%1000, rand()%10000, rand()%1000, rand()%10);
	memset(SeriesInstanceUID,0,sizeof(SOPInstanceUID));
	strcpy_s(SeriesInstanceUID,256,SOPInstanceUID);
	p_ch = strrchr(SeriesInstanceUID,'.');
	memset(p_ch,0,1);	// clear last set of numbers
	p_ch = strrchr(SeriesInstanceUID,'.');
	memset(p_ch,0,1);	// clear second last set of numbers
	sprintf_s(p_ch, SeriesInstanceUID + 256 - p_ch, ".%d%d%d.%d", rand()%1000, rand()%10000, rand()%1000, rand()%10);


	// (0008,xxxx) fields
	strcpy_s(temp, 256, "ORIGINAL\\PRIMARY\\AXIAL\\NANOSPECT");
	len = strlen(temp);
	DE = new DataElement(0x0008,0x0008,"CS",len,temp);	// ImageType
	DCMObj->SetElement(DE);

	strcpy_s(temp, 256,"1.2.840.10008.5.1.4.1.1.2");			// CTImageStorage
	len = strlen(temp);
	DE = new DataElement(0x0008,0x0016,"UI",len,temp);	// SOPClassUID
	DCMObj->SetElement(DE);

	len = strlen(SOPInstanceUID);
	DE = new DataElement(0x0008,0x0018,"UI",len,SOPInstanceUID);	// SOPInstanceUID, generated above
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x0020,temp,256);	// copies date from projections
	DE = new DataElement(0x0008,0x0020,"DA",len,temp);	// StudyDate
	DCMObj->SetElement(DE);
	DE = new DataElement(0x0008,0x0021,"DA",len,temp);	// SeriesDate
	DCMObj->SetElement(DE);
	DE = new DataElement(0x0008,0x0022,"DA",len,temp);	// AcquisitionDate
	DCMObj->SetElement(DE);

	len = strftime(temp,256,"%Y%m%d",timeinfo);			// use current date for ContentDate
	DE = new DataElement(0x0008,0x0023,"DA",len,temp);	// ContentDate
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x002a,temp,256);
	DE = new DataElement(0x0008,0x002a,"DT",len,temp);	// AcquisitionDateTime
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x0030,temp,256);	// copies time from projections
	DE = new DataElement(0x0008,0x0030,"TM",len,temp);	// StudyTime
	DCMObj->SetElement(DE);
	DE = new DataElement(0x0008,0x0031,"TM",len,temp);	// SeriesTime
	DCMObj->SetElement(DE);
	DE = new DataElement(0x0008,0x0032,"TM",len,temp);	// AcquisitionTime
	DCMObj->SetElement(DE);

	len = strftime(temp,256,"%H%M%S.000",timeinfo);		// use actual time for ContentTime
	DE = new DataElement(0x0008,0x0033,"TM",len,temp);	// ContentTime
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x0050,temp,256);
	DE = new DataElement(0x0008,0x0050,"SH",len,temp);	// AccessionNumber
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x0060,temp,256);
	DE = new DataElement(0x0008,0x0060,"CS",2,"CT");	// Modality
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x0070,temp,256);
	DE = new DataElement(0x0008,0x0070,"LO",len,temp);	// Manufacturer
	DCMObj->SetElement(DE);

	strcpy_s(temp,256,"University of Ottawa Heart Institute");
	len  = strlen(temp);
	DE = new DataElement(0x0008,0x0080,"LO",len,temp);	// InstitutionName
	DCMObj->SetElement(DE);

	strcpy_s(temp,256,"40 Ruskin St.\nOttawa,ON\nK1Y 4W7");
	len  = strlen(temp);
	DE = new DataElement(0x0008,0x0081,"ST",len,temp);	// InstitutionAddress
	DCMObj->SetElement(DE);

	DE = new DataElement(0x0008,0x0090,"PN",0,temp);	// ReferringPhysicianName
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x1010,temp,256);
	DE = new DataElement(0x0008,0x1010,"SH",len,temp);	// StationName
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x1030,temp,256);
	DE = new DataElement(0x0008,0x1030,"LO",len,temp);	// StudyDescription
	DCMObj->SetElement(DE);

	strcpy_s(temp,256,"CT recon: with BH corr");
	len = strlen(temp);
	DE = new DataElement(0x0008,0x103e,"LO",len,temp);	// SeriesDescription
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0008,0x1090,temp,256);
	DE = new DataElement(0x0008,0x1090,"LO",len,temp);	// ManufacturersModelName
	DCMObj->SetElement(DE);


	// (0010,xxxx) fields - Patient Info
	len = proj_dcm->GetValue(0x0010,0x0010,temp,256);
	DE = new DataElement(0x0010,0x0010,"PN",len,temp);	// PatientsName
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0010,0x0020,temp,256);
	DE = new DataElement(0x0010,0x0020,"LO",len,temp);	// PatientID
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0010,0x0030,temp,256);
	DE = new DataElement(0x0010,0x0030,"DA",len,temp);	// PatientsBirthDate
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0010,0x0040,temp,256);
	DE = new DataElement(0x0010,0x0040,"CS",len,temp);	// PatientsSex
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0010,0x1020,temp,256);
	DE = new DataElement(0x0010,0x1020,"DS",len,temp);	// PatientsSize
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0010,0x1030,temp,256);
	DE = new DataElement(0x0010,0x1030,"DS",len,temp);	// PatientsWeight
	DCMObj->SetElement(DE);

	// (0018,xxxx) fields - 
	sprintf_s(temp, 256,"%.2f",res);
	len = strlen(temp);
	DE = new DataElement(0x0018,0x0050,"DS",len,temp);	// SliceThickness ***
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x0060,temp,256);
	DE = new DataElement(0x0018,0x0060,"DS",len,temp);	// KVP
	DCMObj->SetElement(DE);

	sprintf_s(temp, 256,"%.2f",-res);
	len = strlen(temp);
	DE = new DataElement(0x0018,0x0088,"DS",len,temp);	// SpacingBetweenSlices ***
	DCMObj->SetElement(DE);

	strcpy_s(temp, 256,"Cone_ct v0.3");
	len = strlen(temp);
	DE = new DataElement(0x0018,0x1020,"LO",len,temp);	// Software
	DCMObj->SetElement(DE);

	strcpy_s(temp, 256,"CT");
	len = strlen(temp);
	DE = new DataElement(0x0018,0x1030,"LO",len,temp);	// ProtocolName
	DCMObj->SetElement(DE);

	sprintf_s(temp, 256,"%.2f",res);
	len = strlen(temp);
	DE = new DataElement(0x0018,0x1050,"DS",len,temp);	// SpatialResolution ***
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x1110,temp,256);
	DE = new DataElement(0x0018,0x1110,"DS",len,temp);	// DistanceSourceToDetector
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x1111,temp,256);
	DE = new DataElement(0x0018,0x1111,"DS",len,temp);	// DistanceSourceToPatient
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x1120,temp,256);
	DE = new DataElement(0x0018,0x1120,"DS",len,temp);	// GantryDetectorTilt
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x1130,temp,256);
	DE = new DataElement(0x0018,0x1130,"DS",len,temp);	// TableHeight
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x1140,temp,256);
	DE = new DataElement(0x0018,0x1140,"CS",len,temp);	// RotationDirection
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x1150,temp,256);
	DE = new DataElement(0x0018,0x1150,"IS",len,temp);	// ExposureTime
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x1152,temp,256);
	DE = new DataElement(0x0018,0x1152,"IS",len,temp);	// Exposure
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x1160,temp,256);
	DE = new DataElement(0x0018,0x1160,"SH",len,temp);	// FilterType
	DCMObj->SetElement(DE);

	/*
	len = proj_dcm->GetValue(0x0018,0x1170,temp,256);
	DE = new DataElement(0x0018,0x1170,"IS",len,temp);	// GeneratorPower
	DCMObj->AddObj(DE);
	*/

	len = proj_dcm->GetValue(0x0018,0x5100,temp,256);
	DE = new DataElement(0x0018,0x5100,"CS",len,temp);	// PatientPosition
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0018,0x8151,temp,256);
	DE = new DataElement(0x0018,0x8151,"DS",len,temp);	// XRayTubeCurrent
	DCMObj->SetElement(DE);

	/*
	len = proj_dcm->GetValue(0x0018,0x9305,temp,256);
	DE = new DataElement(0x0018,0x9305,"FD",len,temp);	// RevolutionTime
	DCMObj->AddObj(DE);
	len = proj_dcm->GetValue(0x0018,0x9306,temp,256);
	DE = new DataElement(0x0018,0x9306,"FD",len,temp);	// SingleCollimationWidth
	DCMObj->AddObj(DE);
	len = proj_dcm->GetValue(0x0018,0x9307,temp,256);
	DE = new DataElement(0x0018,0x9307,"FD",len,temp);	// TotalCollimationWidth
	DCMObj->AddObj(DE);
	len = proj_dcm->GetValue(0x0018,0x9309,temp,256);
	DE = new DataElement(0x0018,0x9309,"FD",len,temp);	// TableSpeed
	DCMObj->AddObj(DE);
	len = proj_dcm->GetValue(0x0018,0x9310,temp,256);
	DE = new DataElement(0x0018,0x9310,"FD",len,temp);	// TableFeedPerRotation
	DCMObj->AddObj(DE);
	len = proj_dcm->GetValue(0x0018,0x9311,temp,256);
	DE = new DataElement(0x0018,0x9311,"FD",len,temp);	// SpiralPitchFactor
	DCMObj->AddObj(DE);
	len = proj_dcm->GetValue(0x0018,0x9327,temp,256);
	DE = new DataElement(0x0018,0x9327,"FD",len,temp);	// TablePosition
	DCMObj->AddObj(DE);
	len = proj_dcm->GetValue(0x0018,0x9345,temp,256);
	DE = new DataElement(0x0018,0x9345,"FD",len,temp);	// CTDIVol
	DCMObj->AddObj(DE);
	*/

	// (0020,xxxx) fields
	len = proj_dcm->GetValue(0x0020,0x000d,temp,256);
	DE = new DataElement(0x0020,0x000d,"UI",len,temp);	// StudyInstanceUID
	DCMObj->SetElement(DE);

	len = strlen(SeriesInstanceUID);
	DE = new DataElement(0x0020,0x000e,"UI",len,SeriesInstanceUID);	// SeriesInstanceUID
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0020,0x0010,temp,256);
	DE = new DataElement(0x0020,0x0010,"SH",len,temp);	// StudyID
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0020,0x0011,temp,256);
	DE = new DataElement(0x0020,0x0011,"IS",len,temp);	// SeriesNumber
	DCMObj->SetElement(DE);

	len = proj_dcm->GetValue(0x0020,0x0012,temp,256);
	DE = new DataElement(0x0020,0x0012,"IS",len,temp);	// AcquisitionNumber
	DCMObj->SetElement(DE);

	len = sprintf_s(temp, 256,"%.1f\\%.1f\\%.1f", (res*cols)/2, (res*rows)/2, (res*slices)/2); // fix this to handle odd numbers
	DE = new DataElement(0x0020,0x0032,"DS",len,temp);	// ImagePositionPatient ***
	DCMObj->SetElement(DE);
	len = sprintf_s(temp, 256,"-1\\0\\0\\0\\-1\\0");
	DE = new DataElement(0x0020,0x0037,"DS",len,temp);	// ImageOrientationPatient ***
	DCMObj->SetElement(DE);
	DE = new DataElement(0x0020,0x0052,"UI",0,temp);	// FrameOfReferenceUID
	DCMObj->SetElement(DE);
	DE = new DataElement(0x0020,0x1040,"LO",0,temp);	// PositionReferenceIndicator
	DCMObj->SetElement(DE);
	len = sprintf_s(temp, 256,"0");
	DE = new DataElement(0x0020,0x1041,"DS",len,temp);	// SliceLocation
	DCMObj->SetElement(DE);

	// (0028,xxxx) fields
	us_temp = 1;
	DE = new DataElement(0x0028,0x0002,"US",sizeof(us_temp),(char*)&us_temp);	// SamplesPerPixel
	DCMObj->SetElement(DE);

	len = sprintf_s(temp, 256, "MONOCHROME2");
	DE = new DataElement(0x0028,0x0004,"CS",len,temp);	// PhotometricInterpretation
	DCMObj->SetElement(DE);

	len = sprintf_s(temp, 256, "%d", slices);
	DE = new DataElement(0x0028,0x0008,"IS",len,temp);	// NumberOfFrames
	DCMObj->SetElement(DE);

	temp[0] = 0x54;
	temp[1] = 0x00;
	temp[2] = 0x80;
	temp[3] = 0x00;
	DE = new DataElement(0x0028,0x0009,"AT",4,temp);	// FrameIncrementPointer
	DCMObj->SetElement(DE);

	us_temp = rows;
	DE = new DataElement(0x0028,0x0010,"US",sizeof(us_temp),(char*)&us_temp);	// Rows
	DCMObj->SetElement(DE);
	us_temp = cols;
	DE = new DataElement(0x0028,0x0011,"US",sizeof(us_temp),(char*)&us_temp);	// Columns
	DCMObj->SetElement(DE);

	len = sprintf_s(temp, 256, "%.2f\\%.2f", res, res);
	DE = new DataElement(0x0028,0x0030,"DS",len,temp);							// PixelSpacing
	DCMObj->SetElement(DE);

	us_temp = 16;
	DE = new DataElement(0x0028,0x0100,"US",sizeof(us_temp),(char*)&us_temp);	// BitsAllocated
	DCMObj->SetElement(DE);
	us_temp = 16;
	DE = new DataElement(0x0028,0x0101,"US",sizeof(us_temp),(char*)&us_temp);	// BitsStored
	DCMObj->SetElement(DE);
	us_temp = 15;
	DE = new DataElement(0x0028,0x0102,"US",sizeof(us_temp),(char*)&us_temp);	// HighBit
	DCMObj->SetElement(DE);
	us_temp = 0;
	DE = new DataElement(0x0028,0x0103,"US",sizeof(us_temp),(char*)&us_temp);	// PixelRepresentation
	DCMObj->SetElement(DE);

	//len = sprintf_s(temp, 256, "0.0");
	//DE = new DataElement(0x0028,0x1052,"DS",len,temp);							// RescaleIntercept
	//DCMObj->SetElement(DE);
	//len = sprintf_s(temp, 256, "1.0");
	//DE = new DataElement(0x0028,0x1053,"DS",len,temp);							// RescaleSlope
	//DCMObj->SetElement(DE);

	// (0054,xxxx)
	p_us = new unsigned short[slices];
	len = slices*2;
	for(i=0; i<slices; i++)
		p_us[i] = i+1;
	DE = new DataElement(0x0054,0x0080,"US",len,(char*)p_us);					// SliceVector
	DCMObj->SetElement(DE);
	delete [] p_us;

	us_temp = slices;
	DE = new DataElement(0x0054,0x0081,"US",sizeof(us_temp),(char*)&us_temp);	// PixelRepresentation
	DCMObj->SetElement(DE);

	// pixel data
	p_us = new unsigned short[slices*rows*cols];
	len = slices*rows*cols*2;
	for(i=0;i<slices;i++)	
		for(j=0;j<rows;j++)
			for(k=0;k<cols;k++)
				p_us[i*rows*cols+j*cols+k] = max(100 * recon[slices-1-i][rows-1-j][k],0.0);  // reverse the slice and row order for the DICOM file
	DE = new DataElement(0x7fe0,0x0010,"OW",len,(char*)p_us);	// PixelRepresentation
	DCMObj->SetElement(DE);
	delete [] p_us;

	f.open(out_file,fstream::binary|fstream::out);
	DCMObj->Write(f);
	f.close();

	delete DCMObj;

	return 0;
}

void Reconstruction::WriteBin(char* out_file)
{
	int i,j;
	ofstream f;

	f.open(out_file,fstream::binary|fstream::out);
	for(i=0;i<slices;i++)
		for(j=0;j<rows;j++)
				f.write(reinterpret_cast<char*>(recon[i][j]),cols*sizeof(FP_VAR));
	f.close();
}

HBITMAP Reconstruction::GetBitmap()
{
	DWORD dwWaitResult;
	int i,j;

	HBITMAP hBMP = NULL;

	float min, max;
	max = 0; min = 0;

	unsigned char temp_us;
	DWORD *pixel_data;
	pixel_data = new DWORD[rows*cols];
	
	dwWaitResult = WaitForSingleObject(hMutex, 1000);
	if(dwWaitResult == WAIT_OBJECT_0)
	{
		// find maximum and minimum
		for(i=0;i<rows;i++)
			for(j=0;j<rows;j++)
			{
				if(display_slice[i][j] > max)
					max = display_slice[i][j];
				//else if(display_slice[i][j] < min)
				//	min = display_slice[i][j];
			}

		for(i=0;i<rows;i++)
			for(j=0;j<rows;j++)
			{
				if(display_slice[i][j] > 0)
					temp_us = 255 * (display_slice[i][j] - min)/(max - min);
				else
					temp_us = 0;
				pixel_data[i*cols + j] = temp_us | (temp_us << 8) | (temp_us << 16);
			}
		ReleaseMutex(hMutex);

		hBMP = CreateBitmap(cols,rows,1,32,pixel_data);
	}

	delete [] pixel_data;
	return hBMP;
}

void Reconstruction::RemoveMetal()
{
	int i,j,k;
	int n;
	FP_VAR*** temp_recon;
	int** temp_proj;
	double YOffset, ZOffset;
	
	double x_r, y_r;		// rotated x,y cooredinates
	double y_p, z_p;	//	projected y,z coordinates
	int fy, fz;
	double dy, dz;
	double scale;

	double cos_theta, sin_theta;

	DWORD dwWaitResult;

	ofstream f;

	// allocate memory
	temp_proj = new int*[proj->rows];
	for(i=0;i<proj->rows;i++)
		temp_proj[i] = new int[proj->cols];

	temp_recon = recon;

	recon = new FP_VAR**[slices];
	for(i=0;i<slices;i++)
		recon[i] = new FP_VAR*[rows];
	for(i=0;i<slices;i++)
		for(j=0;j<rows;j++)
			recon[i][j] = new FP_VAR[cols];

	for(i=0;i<slices;i++)
		for(j=0;j<rows;j++)
			memset(recon[i][j],0,cols*sizeof(FP_VAR));	// initialize memory to zero...
	n=0;

	proj->LoadNextProj();

	while(proj->LoadNextProj())
	{
		// get the angle and offsets
		cos_theta = cos(M_PI*((proj->projAngle)+90)/180);
		sin_theta = sin(M_PI*((proj->projAngle)+90)/180);
		YOffset = proj->getYOffset();
		ZOffset = proj->getZOffset();

		// project the thresholded image into the temporary projections

		// clear the temp projection
		for(i=0;i<proj->rows;i++)
			memset(temp_proj[i],0,proj->cols * sizeof(int));

		for(j=0;j<rows;j++)
		{
			for(k=0;k<cols;k++)
			{
				x_r = x[k] * cos_theta + y[j] * sin_theta;
				y_r = -x[k] * sin_theta + y[j] * cos_theta;
				y_p = y_r * (proj->sourceToDetector/(proj->sourceToAxis + x_r)) + YOffset;		// in mm
				y_p = ((proj->rows-1.0)/2.0) - (y_p/proj->detectorRes);

				// scale = proj->sourceToAxis /(proj->sourceToAxis - x_r);
				// scale *= scale;

				for(i=0;i<slices;i++)
				{
					if(temp_recon[i][j][k] > threshold)
					{
						z_p = z[i] * (proj->sourceToDetector/(proj->sourceToAxis + x_r)) + ZOffset;	// in mm
						z_p = (z_p/proj->detectorRes) + ((proj->cols-1.0)/2.0);
							
						fy = floor(y_p);
						fz = floor(z_p);

						//dy = y_p - fy;
						//dz = z_p - fz;

						if( (fy>0) && (fy < ( proj->rows - 1)) && (fz>0) && (fz<(proj->cols - 1)) )
						{
							temp_proj[fy][fz] = 1; //scale * (1-dy) * (1-dz);// * recon[i][j][k];
							temp_proj[fy+1][fz] = 1; //scale * dy * (1-dz);// * recon[i][j][k];
							temp_proj[fy][fz+1] = 1; //scale * (1-dy);// * dz * recon[i][j][k];
							temp_proj[fy+1][fz+1] = 1; //scale * dy * dz;// * recon[i][j][k];
						}
					}
				}
			}
		}

		f.open("c:\\SPECT\\rat_aorta\\thresh_proj.bin",ios::binary);
		if(f.is_open())
			for(i=0;i<proj->rows;i++)
				f.write(reinterpret_cast<char*>(temp_proj[i]),proj->cols * sizeof(int));
		else
			OutputDebugString(L"Error opening projection output file.");
		f.close();
		
		proj->Interpolate(temp_proj);
		proj->WriteBin("c:\\SPECT\\rat_aorta\\interp_proj.bin");
		proj->Filter();


		for(j=0;j<rows;j++)
		{
			for(k=0;k<cols;k++)
			{
				x_r = x[k] * cos_theta + y[j] * sin_theta;
				y_r = -x[k] * sin_theta + y[j] * cos_theta;
				y_p = y_r * (proj->sourceToDetector/(proj->sourceToAxis + x_r)) + YOffset;		// in mm
				y_p = ((proj->rows-1.0)/2.0) - (y_p/proj->detectorRes);

				scale = proj->sourceToAxis /(proj->sourceToAxis - x_r);
				scale *= scale;

				for(i=0;i<slices;i++)
				{
					
					z_p = z[i] * (proj->sourceToDetector/(proj->sourceToAxis + x_r)) + ZOffset;	// in mm
					z_p = (z_p/proj->detectorRes) + ((proj->cols-1.0)/2.0);
						
					fy = floor(y_p);
					fz = floor(z_p);

					dy = y_p - fy;
					dz = z_p - fz;

					if( (fy>0) && (fy < ( proj->rows - 1)) && (fz>0) && (fz<(proj->cols - 1)) )
						recon[i][j][k] += scale * 
									  (proj->pd[fy][fz] * (1-dy) * (1-dz) +	// bilinear interpolation
									  proj->pd[fy+1][fz] * dy * (1-dz) +
									  proj->pd[fy][fz+1] * (1-dy) * dz +
									  proj->pd[fy+1][fz+1] * dy * dz);

				}
				// check for cancel after each iteration
				if(cancel)
				{
					proj->CloseFindFile();
					// should reset progress bar
					return;
				}
			}
		}
		// copy current recon into display_slice
		dwWaitResult = WaitForSingleObject(hMutex,1000);
		if(dwWaitResult == WAIT_OBJECT_0)
		{
			for(j=0;j<rows;j++)
				for(k=0;k<cols;k++)
					display_slice[j][k] = recon[slices/2][j][k];
			ReleaseMutex(hMutex);
		}
		PostMessage(hApp,WM_UPDATE_RECON,MAKEWPARAM(n,proj->num_proj),NULL);
	}


	// free memory
	for(i=0;i<proj->rows;i++)
		delete [] temp_proj[i];
	delete [] temp_proj;

	
	for(i=0;i<slices;i++)
		for(j=0;j<rows;j++)
			delete [] temp_recon[i][j];
	for(i=0;i<slices;i++)
		delete [] temp_recon[i];
	delete [] temp_recon;

}