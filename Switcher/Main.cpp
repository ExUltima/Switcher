#include "PCH.h"

#include "ApplicationException.h"
#include "Configurations.h"
#include "EngineConfig.h"
#include "EngineProperties.h"
#include "FileUtility.h"
#include "GlobalRef.h"
#include "LoadedEngine.h"
#include "LoadedSwitchType.h"
#include "MainWindow.h"
#include "SwitchTypeProperties.h"
#include "SwitcherContext.h"
#include "TrayIcon.h"

#include "Engine_h.h"

static VOID RaiseEngineManifestActivationError(HRESULT hr, LPCWSTR pszEngineName, LPCWSTR pszManifestFile, LPCWSTR lpManifestResName)
{
	if (!lpManifestResName)
		throw CApplicationException(hr, L"Failed to activate manifest %s for engine %s: 0x%X", pszManifestFile, pszEngineName, hr);
	else if (IS_INTRESOURCE(lpManifestResName))
		throw CApplicationException(hr, L"Failed to activate manifest %u inside %s for engine %s: 0x%X", lpManifestResName, pszManifestFile, pszEngineName, hr);
	else
		throw CApplicationException(hr, L"Failed to activate manifest %s inside %s for engine %s: 0x%X", lpManifestResName, pszManifestFile, pszEngineName, hr);
}

static CComPtr<ISwitchEngine> InstantiateEngine(const CEngineProperties& EngineProps)
{
	const CEngineConfig& Config = EngineProps.GetConfig();
	HRESULT hr;
	HANDLE hActCtx;
	ULONG_PTR uActCookie;
	CComPtr<ISwitchEngine> pEngine;

	// Activate Activation Context.
	const CString& strManifest = EngineProps.GetConfig().GetManifestFile();
	if (!strManifest.IsEmpty())
	{
		ACTCTX Params = { 0 };

		Params.cbSize = sizeof(Params);
		Params.dwFlags = ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
		Params.lpSource = strManifest;
		Params.lpAssemblyDirectory = EngineProps.GetEngineDirectory();

		LPCWSTR lpManifestResName = Config.GetManifestResourceName();
		if (lpManifestResName)
		{
			Params.lpResourceName = lpManifestResName;
			Params.dwFlags |= ACTCTX_FLAG_RESOURCE_NAME_VALID;
		}

		hActCtx = CreateActCtx(&Params);
		if (hActCtx == INVALID_HANDLE_VALUE)
		{
			hr = AtlHresultFromLastError();
			RaiseEngineManifestActivationError(hr, EngineProps.GetEngineName(), strManifest, lpManifestResName);
		}

		try
		{
			if (!ActivateActCtx(hActCtx, &uActCookie))
			{
				hr = AtlHresultFromLastError();
				RaiseEngineManifestActivationError(hr, EngineProps.GetEngineName(), strManifest, lpManifestResName);
			}
		}
		catch (...)
		{
			ATLTRY(ReleaseActCtx(hActCtx));
			throw;
		}
	}
	else
	{
		hActCtx = INVALID_HANDLE_VALUE;
		uActCookie = 0;
	}

	// Instantiate ISwitcherEngine.
	try
	{
		hr = pEngine.CoCreateInstance(Config.GetEngineId(), NULL, CLSCTX_INPROC_SERVER);
		if (FAILED(hr))
			throw CApplicationException(hr, L"Failed to create ISwitcherEngine instance for engine %s: 0x%X", EngineProps.GetEngineName().GetString(), hr);
	}
	catch (...)
	{
		if (hActCtx != INVALID_HANDLE_VALUE)
		{
			try
			{
				if (DeactivateActCtx(0, uActCookie))
					ReleaseActCtx(hActCtx);
			}
			catch (...)
			{
				// Swallow.
			}
		}
		throw;
	}

	if (hActCtx != INVALID_HANDLE_VALUE)
	{
		if (!DeactivateActCtx(0, uActCookie))
		{
			hr = AtlHresultFromLastError();
			throw CApplicationException(hr, L"Failed to deactivate manifest for engine %s: 0x%X.", EngineProps.GetEngineName().GetString(), hr);
		}
		ReleaseActCtx(hActCtx);
	}

	return pEngine;
}

static BOOL LoadEngine(LPCWSTR pszDir, const WIN32_FIND_DATA *pFileDetails, LPVOID pContext)
{
	CEngineMap *pEngines = reinterpret_cast<CEngineMap *>(pContext);
	HRESULT hr;

	if (!(pFileDetails->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return TRUE;

	LPCWSTR pszEngineName = pFileDetails->cFileName;

	CPath EngineDir = pszDir;
	if (!EngineDir.Append(pszEngineName))
		AtlThrow(E_UNEXPECTED);

	// Load engine ID.
	CPath ConfigFile = EngineDir;
	if (!ConfigFile.Append(L"Engine.ini"))
		AtlThrow(E_UNEXPECTED);

	WCHAR szEngineId[64];
	GetPrivateProfileString(L"Engine", L"ID", NULL, szEngineId, _countof(szEngineId), ConfigFile);
	if (!szEngineId[0])
		throw CApplicationException(L"Engine ID is not configured for engine %s.", pszEngineName);

	GUID EngineId;
	hr = CLSIDFromString(szEngineId, &EngineId);
	if (FAILED(hr))
		throw CApplicationException(hr, L"Failed to parse engine ID for engine %s: 0x%X.", pszEngineName, hr);

	CEngineMap::CPair *pEntry = pEngines->Lookup(EngineId);
	if (pEntry)
		throw CApplicationException(L"Engine %s has the same ID as engine %s.", pszEngineName, pEntry->m_value->GetEngineProperties().GetEngineName().GetString());

	// Load engine manifest.
	WCHAR szEngineManifest[MAX_PATH];
	GetPrivateProfileString(L"Engine", L"Manifest", NULL, szEngineManifest, _countof(szEngineManifest), ConfigFile);

	WCHAR szManifestResId[8];
	GetPrivateProfileString(L"Engine", L"ManifestResourceID", NULL, szManifestResId, _countof(szManifestResId), ConfigFile);

	LPCWSTR pszManifestResName;

	if (szManifestResId[0])
	{
		ULONG_PTR uResId = wcstoul(szManifestResId, NULL, 0);
		pszManifestResName = reinterpret_cast<LPCWSTR>(uResId);
		if (!pszManifestResName || !IS_INTRESOURCE(pszManifestResName))
			throw CApplicationException(L"Invalid ManifestResourceID in Engine.ini of %s engine.", pszEngineName);
	}
	else
	{
		WCHAR szManifestResName[256];
		GetPrivateProfileString(L"Engine", L"ManifestResourceName", NULL, szManifestResName, _countof(szManifestResName), ConfigFile);
		pszManifestResName = szManifestResName[0] ? szManifestResName : NULL;
	}

	// Instantiate Engine Properties.
	CAutoPtr<CEngineConfig> pConfig(new CEngineConfig(EngineId, szEngineManifest, pszManifestResName));
	CAutoPtr<CEngineProperties> pEngineProps(new CEngineProperties(pConfig, pszEngineName, EngineDir));

	// Instantiate Engine.
	CAutoPtr<CLoadedEngine> pLoaded(new CLoadedEngine(pEngineProps, InstantiateEngine(*pEngineProps)));
	pEngines->SetAt(EngineId, pLoaded);

	return TRUE;
}

static CAutoPtr<CEngineMap> LoadEngines()
{
	// Construct the path of directory that contains engines.
	CPath EnginesDir = _Module.GetModuleDirectory();
	if (!EnginesDir.Append(ENGINES_DIRECTORY))
		AtlThrow(E_UNEXPECTED);

	// List all engines.
	CAutoPtr<CEngineMap> pEngines(new CEngineMap());
	EnumerateFiles(EnginesDir, LoadEngine, pEngines.m_p);

	if (pEngines->GetCount() == 0)
		throw CApplicationException(L"There is no any engines installed. Please install at least one engine before using " APP_NAME ".");

	return pEngines;
}

static BOOL LoadSwitchType(LPCWSTR pszDir, const WIN32_FIND_DATA *pFileDetails, LPVOID pContext)
{
	auto stypes = reinterpret_cast<switch_type_map *>(pContext);
	HRESULT hr;
	WCHAR guid[64];

	if (!(pFileDetails->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return TRUE;

	// Setup file name.
	auto sname = pFileDetails->cFileName;

	std::tr2::sys::path sdir(pszDir);
	sdir.append(sname);

	std::tr2::sys::path cfile(sdir);
	cfile.append(L"Switch.ini");

	// Load switch ID.
	GetPrivateProfileString(L"Switch", L"ID", NULL, guid, _countof(guid), cfile.c_str());
	if (!guid[0])
		throw CApplicationException(L"Switch ID is not configured for switch %s.", sname);

	GUID sid;
	hr = CLSIDFromString(guid, &sid);
	if (FAILED(hr))
		throw CApplicationException(hr, L"Failed to parse switch ID for switch %s: 0x%X.", sname, hr);

	auto it = stypes->find(sid);
	if (it != stypes->cend())
		throw CApplicationException(L"Switch %s has the same ID as switch %s.", sname, it->second->properties()->GetName());

	// Load engine ID.
	GetPrivateProfileString(L"Switch", L"Engine", NULL, guid, _countof(guid), cfile.c_str());
	if (!guid[0])
		throw CApplicationException(L"Engine ID is not configured for switch %s.", sname);

	GUID eid;
	hr = CLSIDFromString(guid, &eid);
	if (FAILED(hr))
		throw CApplicationException(hr, L"Failed to parse engine ID for switch %s: 0x%X.", sname, hr);

	auto *engine = g_pEngines->Lookup(eid);
	if (!engine)
		throw CApplicationException(L"Switch %s is configured to use the non-existent engine.", sname);

	// Instantiate SwitchTypeConfig & SwitchTypeProperties.
	auto conf = CreateComObject<SwitchTypeConfig>();
	conf->SetEngineId(eid);
	conf->SetSwitchTypeId(sid);

	auto props = CreateComObject<SwitchTypeProperties>();
	props->SetName(sname);
	props->SetConfig(conf);
	props->SetDirectory(sdir.c_str());

	// Loade Switch.
	CComPtr<ISwitchType> stype;
	hr = engine->m_value->GetEngine()->LoadSwitchType(props, &stype);
	if (FAILED(hr))
		ThrowLastErrorInfo(L"Failed to load switch %s: 0x%X", sname, hr);

	auto loaded = std::make_shared<loaded_switch_type>(stype, props);
	auto res = stypes->insert(std::make_pair(sid, loaded));

	if (!res.second)
		throw CApplicationException(L"Failed to store switch %s in the memory. Please contact developer.", sname);

	return TRUE;
}

static std::shared_ptr<switch_type_map> load_switch_types()
{
	std::experimental::filesystem::path sdir = _Module.GetModuleDirectory();
	sdir.append(SWITCHES_DIRECTORY);

	auto stypes = std::make_shared<switch_type_map>();
	EnumerateFiles(sdir.c_str(), LoadSwitchType, stypes.get());
	return stypes;
}

static std::shared_ptr<CMainWindow> create_window(const switcher_context& ctx)
{
	auto wnd = std::make_shared<CMainWindow>();
	
	if (!wnd->Create(nullptr, nullptr, nullptr, 0, 0, nullptr, const_cast<switcher_context *>(&ctx)))
		AtlThrowLastWin32();

	return wnd;
}

static void run_window(switcher_context& ctx)
{
	CMessageLoop messageLoop;

	if (!_Module.AddMessageLoop(&messageLoop))
		AtlThrow(E_UNEXPECTED);

	try
	{
		// Initialize.
		INITCOMMONCONTROLSEX comctls = { 0 };
		comctls.dwSize = sizeof(comctls);
		comctls.dwICC = ICC_WIN95_CLASSES | ICC_DATE_CLASSES | ICC_USEREX_CLASSES | ICC_COOL_CLASSES |
						ICC_INTERNET_CLASSES | ICC_PAGESCROLLER_CLASS | ICC_NATIVEFNTCTL_CLASS |
						ICC_STANDARD_CLASSES | ICC_LINK_CLASS;

		if (!InitCommonControlsEx(&comctls))
			throw CApplicationException(L"Failed to initialize common controls. Please contact developer.");

		// Create Main Window.
		auto wnd = create_window(ctx);
		ctx.set_main_window(wnd);

		// Enter main loop.
		// Don't destroy main window in case of exception since we don't known it is in good state or not.
		EXIT_TYPE nExitType;
		nExitType = static_cast<EXIT_TYPE>(messageLoop.Run());
	}
	catch (...)
	{
		_Module.RemoveMessageLoop();
		throw;
	}

	if (!_Module.RemoveMessageLoop())
		AtlThrow(E_UNEXPECTED);
}

static void run(switcher_context& ctx)
{
	// Load Switch Engines.
	CGlobalRef<CEngineMap> pEngines(g_pEngines);
	pEngines = LoadEngines().Detach();

	// Load Switch Types.
	auto stypes = load_switch_types();
	ctx.set_switch_types(stypes);

	// Run Main Loop.
	run_window(ctx);
}

static HRESULT RunWTL(HINSTANCE hInstance)
{
	HRESULT hr;

	hr = _Module.Init(NULL, hInstance);
	if (FAILED(hr))
		AtlThrow(hr);

	try
	{
		switcher_context ctx;
		run(ctx);
		hr = S_OK;
	}
	catch (CApplicationException& e)
	{
		MessageBox(NULL, e.GetMessage(), APP_NAME, MB_OK | MB_ICONHAND);
		hr = e.GetErrorCode();
	}
	catch (...)
	{
		_Module.Term();
		throw;
	}

	_Module.Term();

	return hr;
}

EXTERN_C INT CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE /* hPrevInstance */, LPWSTR /* lpCmdLine */, INT /* nCmdShow */)
{
	HRESULT hr;

	// Turn on memory leak reporting for debug build.
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

	// initialize CRT.
	std::srand(GetTickCount());

	// Initialize COM.
	hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr))
	{
		MessageBox(NULL, L"Failed to initialize COM. Please contact developer.", APP_NAME, MB_OK | MB_ICONHAND);
		return hr;
	}

	// Run Application.
	try
	{
		hr = RunWTL(hInstance);
	}
	catch (CAtlException& e)
	{
		MessageBox(NULL, L"Unexpected error occurred on ATL or WTL. Please contact developer.", APP_NAME, MB_OK | MB_ICONHAND);
		hr = e;
	}
	catch (...)
	{
		CoUninitialize();
		throw;
	}

	// Clean up.
	CoUninitialize();

	return hr;
}
