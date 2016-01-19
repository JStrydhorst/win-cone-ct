#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <strsafe.h>
//#include <stdio.h>

#include "basewindow.h"
#include "resource.h"

#include "dicom.h"
#include "ct_recon_win.h"

INT_PTR WINAPI AboutDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK DimBoxProc(HWND, UINT uMsg, WPARAM wParam, LPARAM lParam);

class MainWindow : public BaseWindow<MainWindow>
{
public:
	MainWindow();
	~MainWindow();

	PCWSTR ClassName() const { return L"Sample Window Class"; }
	LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

	BOOL OpenProjData();
	BOOL SetDimText();
	BOOL StartReconstruction();
	BOOL SaveRecon();
	BOOL SaveDicom();
	BOOL LoadRecon();
	BOOL RemoveMetal();

	VOID UpdateDisplay();

	void CreateMainWindow();
private:
	HFONT m_hFontNormal;		// Normal Font (9pt Segoe UI)
	HBRUSH m_hbrBackground;		// Used to paint the control backgrounds

	HWND m_hLoadProj;			// load projections button
	HWND m_hCurrentFolder;		// static control showing current folder

	HWND m_hReconSettings;		// reconstruction dimension group box
	HWND m_hReconText[6];		// text in edit box
	HWND m_hDimensions[3];		// edit controls
	HWND m_hFilter;				// filter group box
	HWND m_hCutoff;				// filter selection buttons
	HWND m_hBeamHardening;		// beam hardeining checkbox

	HWND m_hMetalThreshold;		

	HWND m_hReconstruct;
	HWND m_hCancel;
	HWND m_hSave;
	HWND m_hLoad;
	HWND m_hRemoveMetal;

	HBITMAP m_hbmpSlice;
	HBITMAP m_hbmpProj;
	HWND m_hProgress;

	Projection *m_Proj;
	Reconstruction *m_Recon;
	BOOL m_ReconSaved;

	uintptr_t m_thrRecon;	// reconstruction thread
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
	MainWindow win;

	if(!win.Create(L"Cone-Beam CT Reconstruction", WS_OVERLAPPEDWINDOW | WS_EX_CONTROLPARENT))
	{
		return 0;
	}

	ShowWindow(win.Window(), nCmdShow);

	MSG msg = {};
	BOOL bRet;
	while(bRet = GetMessage(&msg, NULL, 0, 0))
	{
		if(bRet == -1)
		{
			return -1;
		}
		else
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return msg.wParam;
}

MainWindow::MainWindow()
{
	m_Proj = NULL;
	m_Recon = NULL;
	m_ReconSaved = FALSE;
	m_hbmpSlice = NULL;
}

MainWindow::~MainWindow()
{
	DeleteObject(m_hFontNormal);

	if(m_Proj)
		delete m_Proj;
	if(m_Recon)
		delete m_Recon;

}

LRESULT MainWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_CREATE:
		CreateMainWindow();
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(m_hwnd, &ps);
			FillRect(hdc, &ps.rcPaint, (HBRUSH) (COLOR_WINDOW+1));
			if(m_hbmpSlice)
				DrawState(hdc,NULL,NULL,(LPARAM)m_hbmpSlice,0,400,46,0,0,DST_BITMAP);
			EndPaint(m_hwnd, &ps);
		}
		return 0;

	case WM_UPDATE_RECON:
		UpdateDisplay();
		SendMessage(m_hProgress, PBM_SETPOS, (WPARAM)LOWORD(wParam),NULL);
		return 0;

	case WM_RECON_COMPLETE:
		ShowWindow(m_hProgress, SW_HIDE);
		m_ReconSaved = false;
		return 0;

	case WM_COMMAND:
		if(lParam) // Control
		{
			switch(LOWORD(wParam))
			{
			case ID_RECON_DIM:
				if(HIWORD(wParam)==EN_CHANGE)
					SetDimText();
				return 0;
			case ID_LOAD_PROJ:
				if(HIWORD(wParam)==BN_CLICKED)
					OpenProjData();
				return 0;
			case ID_START_RECON:
				if(HIWORD(wParam)==BN_CLICKED)
				{
					ShowWindow(m_hProgress, SW_SHOW);
					StartReconstruction();
					return 0;
				}
			case ID_CANCEL_RECON:
				if(HIWORD(wParam)==BN_CLICKED)
				{
					m_Recon->CancelRecon();
					ShowWindow(m_hProgress, SW_HIDE);
				}
				return 0;
			case ID_SAVE_RECON:
				if(HIWORD(wParam)==BN_CLICKED)
				{
					SaveRecon();
					//SaveDicom();
				}
				return 0;
			case ID_LOAD_RECON:
				if(HIWORD(wParam)==BN_CLICKED)
					LoadRecon();
				return 0;
			case ID_REMOVE_METAL:
				if(HIWORD(wParam)==BN_CLICKED)
					RemoveMetal();
				return 0;
			}
			break;
		}
		else
		{
			switch(HIWORD(wParam))
			{
			case 0:	// Menu
				switch(LOWORD(wParam))
				{
				case IDM_OPEN:
					OpenProjData();
					return 0;
				case IDM_EXIT:
					DestroyWindow(m_hwnd);
					return 0;
				case IDM_ABOUT:
					DialogBox(GetModuleHandle(NULL),L"AboutBox",m_hwnd,(DLGPROC)AboutDlgProc);
					return 0;
				}
				break;
			case 1:		// Accelerator
				break;
			}
		}
		break;

	case WM_HSCROLL:
		if((HWND)lParam==m_hCutoff)
		{
			switch(LOWORD(wParam))
			{
			case TB_THUMBTRACK:
				if(SendMessage(m_hCutoff,TBM_GETPOS,0,0)==0)
					SendMessage(m_hCutoff,TBM_SETPOS,TRUE,1);
			}
			return 0;
		}
		break;

	case WM_CTLCOLORSTATIC: // ensures the background of the controls is drawn in white
		{
			HDC hDC = (HDC) wParam;
			SetBkColor(hDC,RGB(255,255,255));
			return (INT_PTR)m_hbrBackground;
		}

	}
	
	return DefWindowProc(m_hwnd, uMsg, wParam, lParam);

	// return TRUE;
}

void MainWindow::CreateMainWindow()
{
	OSVERSIONINFO osvi = {0};
	NONCLIENTMETRICS ncm = {0};

	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

	GetVersionEx(&osvi);
	if(osvi.dwMajorVersion < 6)
		ncm.cbSize = sizeof(NONCLIENTMETRICS) - 4;
	else
		ncm.cbSize = sizeof(NONCLIENTMETRICS);
	SystemParametersInfo(SPI_GETNONCLIENTMETRICS,ncm.cbSize,&ncm,0);

	m_hFontNormal = CreateFontIndirect(&ncm.lfMessageFont);
	m_hbrBackground = (HBRUSH)(COLOR_WINDOW+1);

	SetMenu(m_hwnd,LoadMenu(GetModuleHandle(NULL),L"WCCMenu"));

	m_hLoadProj = CreateWindow(L"Button",L"Open projection data...",
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT | CCS_ADJUSTABLE,
		10, 10,
		200, 23,
		m_hwnd,
		(HMENU)ID_LOAD_PROJ,
		NULL, 0);
	SendMessage(m_hLoadProj, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hCurrentFolder = CreateWindow(L"Static", L"No projection data selected",
		WS_CHILD | WS_VISIBLE,
		220, 15,
		200, 15,
		m_hwnd,
		NULL, NULL, 0);
	SendMessage(m_hCurrentFolder, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hReconSettings = CreateWindow(L"Button",
		L"Reconstruction settings:",
		WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
		10, 250,
		200, 285,
		m_hwnd,
		NULL, NULL, 0);
	SendMessage(m_hReconSettings, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	for(int i=0;i<3;i++)
	{
		m_hDimensions[i] = CreateWindowEx(WS_EX_CLIENTEDGE,
			L"Edit",
			L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT,
			120, 268+30*i,
			40, 23,
			m_hwnd,
			(HMENU)ID_RECON_DIM,
			NULL, 0);
			SendMessage(m_hDimensions[i], WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));
			SendMessage(m_hDimensions[i], EM_SETLIMITTEXT, (WPARAM)4, NULL);
	}

	m_hReconText[0] = CreateWindow(L"Static",
		L"X/Y Pixels:",
		WS_CHILD | WS_VISIBLE,
		19, 271,
		85, 15,
		m_hwnd,
		NULL, NULL, 0);

	m_hReconText[1] = CreateWindow(L"Static",
		L"Z Slices:",
		WS_CHILD | WS_VISIBLE,
		19, 301,
		85, 15,
		m_hwnd,
		NULL, NULL, 0);

	m_hReconText[2] = CreateWindow(L"Static",
		L"Resolution (mm):",
		WS_CHILD | WS_VISIBLE,
		19, 331,
		100, 15,
		m_hwnd,
		NULL, NULL, 0);

	m_hReconText[3] = CreateWindow(L"Static",
		L"Filter:",
		WS_CHILD | WS_VISIBLE,
		19, 363,
		50, 15,
		m_hwnd,
		NULL, NULL, 0);

	m_hReconText[4] = CreateWindow(L"Static",
		L"Cutoff:",
		WS_CHILD | WS_VISIBLE,
		19, 409,
		50, 15,
		m_hwnd,
		NULL, NULL, 0);

	m_hReconText[5] = CreateWindow(L"Static",
		L"",
		WS_CHILD | WS_VISIBLE,
		19, 499,
		175, 30,
		m_hwnd,
		NULL, NULL, 0);

	for(int i=0;i<6;i++)
		SendMessage(m_hReconText[i], WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	SendMessage(m_hDimensions[0], WM_SETTEXT, 0, (WPARAM)L"128");
	SendMessage(m_hDimensions[1], WM_SETTEXT, 0, (WPARAM)L"128");
	SendMessage(m_hDimensions[2], WM_SETTEXT, 0, (WPARAM)L"0.1");


	m_hFilter = CreateWindow(L"ComboBox",
		L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
		19, 379,
		150, 23,
		m_hwnd,
		NULL, NULL, 0);
	SendMessage(m_hFilter, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));
	SendMessage(m_hFilter, CB_ADDSTRING, 0, (LPARAM)L"Ram-Lak");
	SendMessage(m_hFilter, CB_ADDSTRING, 0, (LPARAM)L"Shepp-Logan");
	SendMessage(m_hFilter, CB_ADDSTRING, 0, (LPARAM)L"Hamming");
	SendMessage(m_hFilter, CB_ADDSTRING, 0, (LPARAM)L"Hann");
	SendMessage(m_hFilter, CB_ADDSTRING, 0, (LPARAM)L"Cosine");
	SendMessage(m_hFilter, CB_SETCURSEL, 0, NULL);

	m_hCutoff = CreateWindowEx(0, 
        TRACKBAR_CLASS,                 
        L"Trackbar Control",             
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOP,   
        19, 425,                         
        150, 30,                        
        m_hwnd,							 
        NULL, NULL, NULL); 
	SendMessage(m_hCutoff, TBM_SETRANGE,(WPARAM)FALSE,MAKELPARAM(0,10));
	SendMessage(m_hCutoff, TBM_SETPOS,(WPARAM)TRUE,LPARAM(10));

	m_hBeamHardening = CreateWindowEx(0,
		L"Button",
		L"Beam hardening correction",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | WS_DISABLED,
		19, 462,
		170, 30,
		m_hwnd,
		NULL, NULL, NULL);
	SendMessage(m_hBeamHardening, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hReconstruct = CreateWindow(L"Button",
		L"Start reconstruction",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP |BS_PUSHBUTTON,
		217, 250,
		150, 23,
		m_hwnd,
		(HMENU)ID_START_RECON,
		NULL, NULL);
	SendMessage(m_hReconstruct, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hCancel = CreateWindow(L"Button",
		L"Cancel",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		217, 280,
		150, 23,
		m_hwnd,
		(HMENU)ID_CANCEL_RECON,
		NULL, NULL);
	SendMessage(m_hCancel, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hSave = CreateWindow(L"Button",
		L"Save reconstruction",
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		217, 310,
		150, 23,
		m_hwnd,
		(HMENU)ID_SAVE_RECON,
		NULL, NULL);
	SendMessage(m_hSave, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hLoad = CreateWindow(L"Button",
		L"Load reconstruction",
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		217, 340,
		150, 23,
		m_hwnd,
		(HMENU)ID_LOAD_RECON,
		NULL, NULL);
	SendMessage(m_hLoad, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hRemoveMetal = CreateWindow(L"Button",
		L"Remove metal",
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		217, 375,
		150, 23,
		m_hwnd,
		(HMENU)ID_REMOVE_METAL,
		NULL, NULL);
	SendMessage(m_hRemoveMetal, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hMetalThreshold = CreateWindowEx(WS_EX_CLIENTEDGE,
		L"Edit",
		L"",
		WS_CHILD | WS_VISIBLE | ES_LEFT,
		217, 405,
		30, 23,
		m_hwnd,
		NULL, NULL, NULL);
	SendMessage(m_hMetalThreshold, WM_SETFONT, (WPARAM)m_hFontNormal, MAKELPARAM(TRUE,0));

	m_hProgress = CreateWindowEx(0, PROGRESS_CLASS, (LPWSTR)NULL,
		WS_CHILD,
		430, 19,
		200, 23,
		m_hwnd,
		NULL, NULL, NULL);
}

BOOL MainWindow::OpenProjData()
{
	RootDicomObj *DCMinfo = NULL;
	char filename[MAX_PATH];
	char projFolder[MAX_PATH];

	BROWSEINFO bi = {0};
	PIDLIST_ABSOLUTE pidl;

	WCHAR szDisplayName[MAX_PATH];
	WCHAR szTitle[] = L"Select a folder containing projection data.";

	HANDLE hFindFile;
	WCHAR szFileName[MAX_PATH];
	WIN32_FIND_DATA findData = {0};

	if(m_Recon && !m_ReconSaved)
		if(MessageBox(m_hwnd,
			L"Reconstruction has not been saved. Do you wish to continue?",
			L"WinCone CT",
			MB_ICONWARNING | MB_YESNO) == IDNO)
			return FALSE;

	bi.hwndOwner = m_hwnd;
	bi.pidlRoot = NULL;
	bi.pszDisplayName = szDisplayName;
	bi.lpszTitle = szTitle;
	bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON | BIF_RETURNONLYFSDIRS;
	bi.lpfn = NULL;

	if(!(pidl = SHBrowseForFolder(&bi)))
		return FALSE;

	SHGetPathFromIDList(pidl, szDisplayName);

	// do _findfirst() stuff with current directory
	StringCchPrintf(szFileName,MAX_PATH,L"%s\\%s",szDisplayName,L"1.3.6.1.4.1*");
	hFindFile = FindFirstFile(szFileName,&findData);

	if(hFindFile == INVALID_HANDLE_VALUE)
	{
		MessageBox(m_hwnd,
			L"The selected folder does not contain valid DICOM projection data.",
			L"Load Projection Data",
			MB_OK);
		return FALSE;
	}

	FindClose(hFindFile);

	sprintf_s(filename,sizeof(filename),"%S\\%S",szDisplayName,findData.cFileName);
	DCMinfo = new RootDicomObj(filename,true);
	// confirm that DICOM files are legit here

	if(m_Recon)
	{
		delete m_Recon;
		m_Recon = NULL;
	}
	if(m_Proj)
	{
		delete m_Proj;
		m_Proj = NULL;
	}

	// Update the current folder field
	SendMessage(m_hCurrentFolder,WM_SETTEXT,NULL,(LPARAM)szDisplayName);

	// Enable the reconstruction controls

	delete DCMinfo;

	WideCharToMultiByte(1251,WC_NO_BEST_FIT_CHARS,szDisplayName,MAX_PATH,projFolder,sizeof(projFolder),0,NULL);
	m_Proj = new Projection(projFolder);
	SendMessage(m_hProgress,PBM_SETRANGE,MAKEWPARAM(0,0),MAKELPARAM(0,m_Proj->GetNumProj()));

	return TRUE;
}

BOOL MainWindow::SetDimText()
{
	WCHAR szText[64];
	WCHAR szText2[32];
	INT nxy, nz;
	FLOAT res, size_xy, size_z;
	SendMessage(m_hDimensions[0],WM_GETTEXT,64,(LPARAM)szText);
	nxy = _wtoi(szText);
	SendMessage(m_hDimensions[1],WM_GETTEXT,64,(LPARAM)szText);
	nz = _wtoi(szText);
	SendMessage(m_hDimensions[2],WM_GETTEXT,64,(LPARAM)szText);
	res = _wtof(szText);

	size_xy = nxy * res;
	size_z = nz * res;
	if(size_xy>1000)
	{
		size_xy /= 1000.0;
		StringCchPrintf(szText,64,L"Reconstruction volume:\n%1.3f m \u00D7 %1.3f m ", size_xy, size_xy);
	}
	else
		StringCchPrintf(szText,64,L"Reconstruction volume:\n%3.1f mm \u00D7 %3.1f mm ", size_xy, size_xy);
	if(size_z>1000)
	{
		size_z /= 1000.0;
		StringCchPrintf(szText2,32,L"\u00D7 %1.3f m", size_z);
	}
	else
		StringCchPrintf(szText2,32,L"\u00D7 %3.1f mm", size_z);
	StringCchCat(szText,sizeof(szText),szText2);
	SendMessage(m_hReconText[5],WM_SETTEXT,NULL,(LPARAM)szText);

	return TRUE;
}

BOOL MainWindow::StartReconstruction()
{
	// read in variables from GUI
	WCHAR szText[8];
	INT nxy, nz;
	FLOAT res, cutoff;
	filter_type filter;

	SendMessage(m_hDimensions[0],WM_GETTEXT,64,(LPARAM)szText);
	nxy = _wtoi(szText);
	SendMessage(m_hDimensions[1],WM_GETTEXT,64,(LPARAM)szText);
	nz = _wtoi(szText);
	SendMessage(m_hDimensions[2],WM_GETTEXT,64,(LPARAM)szText);
	res = _wtof(szText);

	filter = (filter_type)SendMessage(m_hFilter,CB_GETCURSEL,NULL,NULL);
	cutoff = SendMessage(m_hCutoff,TBM_GETPOS,NULL,NULL)/10.0;

	m_Recon = new Reconstruction(nz, nxy, nxy, res, m_Proj);
	m_Recon->SetHWND(m_hwnd);
	m_Proj->CreateFilter(filter,cutoff);

	m_thrRecon = _beginthreadex(NULL, 0, Reconstruction::ReconThread, m_Recon, 0, NULL);
	if(!m_thrRecon)
		return FALSE;

	return TRUE;
}

BOOL MainWindow::RemoveMetal()
{
	float threshold;
	WCHAR szThresh[16];					
	filter_type filter;
	FLOAT cutoff;

	filter = (filter_type)SendMessage(m_hFilter,CB_GETCURSEL,NULL,NULL);
	cutoff = SendMessage(m_hCutoff,TBM_GETPOS,NULL,NULL)/10.0;
	m_Proj->CreateFilter(filter,cutoff);

	SendMessage(m_hMetalThreshold, WM_GETTEXT, (WPARAM)16,(LPARAM)szThresh);
	threshold = _wtof(szThresh);

	m_Recon->SetMetalThreshold(threshold);

	m_thrRecon = _beginthreadex(NULL, 0, Reconstruction::RemoveMetalThread, m_Recon, 0, NULL);
	if(!m_thrRecon)
		return FALSE;

	return TRUE;

}

BOOL MainWindow::SaveRecon()
{

	WCHAR szFilename[MAX_PATH] = L"";
	WCHAR szInitialDir[MAX_PATH] = {0};

	SendMessage(m_hCurrentFolder, WM_GETTEXT, (WPARAM)MAX_PATH, (LPARAM)szInitialDir);

	char filename[MAX_PATH];

	OPENFILENAME ofn = {0};
	
	ofn.lStructSize = sizeof(OPENFILENAME);
	ofn.hwndOwner = m_hwnd;
	ofn.lpstrFile = szFilename;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrInitialDir = szInitialDir;
	if(GetSaveFileName(&ofn))
	{	
		WideCharToMultiByte(1251,WC_NO_BEST_FIT_CHARS,szFilename,MAX_PATH,filename,sizeof(filename),0,NULL);
		m_Recon->WriteBin(filename);

		m_ReconSaved = true;

		return TRUE;
	}

	return FALSE;
}

BOOL MainWindow::SaveDicom()
{
	WCHAR szFilename[MAX_PATH] = L"";
	WCHAR szInitialDir[] = L"C:\\SPECT";

	char filename[MAX_PATH];

	OPENFILENAME ofn = {0};
	
	ofn.lStructSize = sizeof(OPENFILENAME);
	ofn.hwndOwner = m_hwnd;
	ofn.lpstrFile = szFilename;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrInitialDir = szInitialDir;
	if(GetSaveFileName(&ofn))
	{	
		WideCharToMultiByte(1251,WC_NO_BEST_FIT_CHARS,szFilename,MAX_PATH,filename,sizeof(filename),0,NULL);
		m_Recon->WriteDicom(filename);

		return TRUE;
	}

	return FALSE;

}

BOOL MainWindow::LoadRecon()
{
	WCHAR szFilename[MAX_PATH] = L"";
	WCHAR szInitialDir[] = L"C:\\SPECT";
	WCHAR szText[64];
	
	int slices;
	int rows;
	int cols;
	float res;
	char filename[MAX_PATH];

	OPENFILENAME ofn = {0};
	
	ofn.lStructSize = sizeof(OPENFILENAME);
	ofn.hwndOwner = m_hwnd;
	ofn.lpstrFile = szFilename;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrInitialDir = szInitialDir;
	GetOpenFileName(&ofn);

	SendMessage(m_hDimensions[0],WM_GETTEXT,64,(LPARAM)szText);
	rows = cols = _wtoi(szText);
	SendMessage(m_hDimensions[1],WM_GETTEXT,64,(LPARAM)szText);
	slices = _wtoi(szText);
	SendMessage(m_hDimensions[2],WM_GETTEXT,64,(LPARAM)szText);
	res = _wtof(szText);

	WideCharToMultiByte(1251,WC_NO_BEST_FIT_CHARS,szFilename,MAX_PATH,filename,sizeof(filename),0,NULL);

	if(m_Recon)
		delete m_Recon;
	m_Recon = new Reconstruction(slices, rows, cols, res, m_Proj, filename);
	m_Recon->SetHWND(m_hwnd);

	return TRUE;
}

VOID MainWindow::UpdateDisplay()
{
	HDC hdc;
	// convert display_slice to bitmap
	if(m_hbmpSlice)
		DeleteObject(m_hbmpSlice);

	m_hbmpSlice = m_Recon->GetBitmap();
	if(m_hbmpSlice)
	{
		hdc = GetDC(m_hwnd);
		DrawState(hdc,NULL,NULL,(LPARAM)m_hbmpSlice,0,400,46,0,0,DST_BITMAP);
		ReleaseDC(m_hwnd, hdc);
	}
}

INT_PTR WINAPI AboutDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		return TRUE;
	case WM_COMMAND:
		switch(wParam)
		{
		case IDOK:
			EndDialog(hwnd,0);
			return TRUE;
		}
	}
	return FALSE;
}
