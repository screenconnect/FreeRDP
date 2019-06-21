#pragma comment(lib,  "ws2_32.lib")

#include <winsock2.h>
#include <ws2tcpip.h>
#include <assert.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/bitmap.h>
#include <freerdp/freerdp.h>

typedef struct bridge_context bridgeContext;
static BOOL flip_bitmap(const BYTE*, BYTE*, UINT32, UINT32);
static BOOL bridge_register_pointer(rdpGraphics*);
static BOOL bridge_pointer_new(rdpContext*, const rdpPointer*);
static BOOL bridge_pointer_free(rdpContext*, rdpPointer*);
static BOOL bridge_pointer_set(rdpContext*, const rdpPointer*);
static BOOL bridge_pointer_set_null(rdpContext*);
static BOOL bridge_pointer_set_default(rdpContext*);
static BOOL bridge_pointer_set_position(rdpContext*, UINT32, UINT32);
static BOOL bridge_begin_paint(rdpContext* context);
static BOOL bridge_end_paint(rdpContext* context);

freerdp* initiate_bridge_connect(char*, int, char*, char*, char*, UINT32, UINT32);
__declspec(dllexport) void* __cdecl bridge_create_freerdp_connection(char*, char*, int, char*, char*, UINT32, UINT32);
__declspec(dllexport) bridgeContext __cdecl bridge_try_get_update(void*);
__declspec(dllexport) bridgeContext __cdecl bridge_get_current_context(void*);
__declspec(dllexport) BOOL __cdecl bridge_freerdp_send_keyboard_event(void*, UINT32, BOOL, BOOL);
__declspec(dllexport) BOOL __cdecl bridge_freerdp_send_mouse_event(void*, UINT16, UINT16, UINT16);
__declspec(dllexport) BOOL __cdecl bridge_cleanup(void*, BOOL);
__declspec(dllexport) BOOL __cdecl bridge_freerdp_connection_possible(char*, char*, int, char*, char*);

struct bridge_context
{
	rdpContext context;
	HANDLE thread;

	BOOL connectionFailure;
	BOOL updatesReceived;

	UINT32 bmFormat;
	UINT32 bmWidth;
	UINT32 bmHeight;
	UINT32 bmScanline;
	BYTE* bmData;

	//UINT32 cursorXpos;
	//UINT32 cursorYpos;

	//BOOL fIcon;
	//DWORD xHotspot;
	//DWORD yHotspot;
	//HBITMAP hbmMask;
	//HBITMAP hbmColor;
};

// this is the entry point for .NET to initiate a new connection
// .NET will repeatedly pass pointer to freerdp struct back to bridge_try_get_update
//   pass (0,0) for width, height to use default settings (1024,768)
void* bridge_create_freerdp_connection(char* hostname, char* domain, int port, char* user, char* password, UINT32 desktopWidth, UINT32 desktopHeight)
{
	freerdp* instance = initiate_bridge_connect(hostname, port, domain, user, password, desktopWidth, desktopHeight);
	return instance;
}

// we'll call this in a loop from C# to get an update
//   if we don't get an update for whatever reason, return bridgeContext with connectionFailure set to TRUE
//   .NET will need to call a cleanup method when it receives a bridgeContext with connectionFailure set to TRUE
bridgeContext bridge_try_get_update(void* inst)
{
	HANDLE handles[64];
	DWORD nCount;
	freerdp* instance;
	rdpContext* context;
	rdpSettings* settings;

	instance = (freerdp*)inst;
	context = instance->context;
	settings = instance->settings;

	bridgeContext* bc = (bridgeContext*)context;

	bc->updatesReceived = FALSE;

	nCount = 0;
	DWORD tmp = freerdp_get_event_handles(context, &handles[nCount], 64 - nCount);

	if (tmp == 0)
		goto fail;

	nCount += tmp;

	if (MsgWaitForMultipleObjects(nCount, handles, FALSE, 1000, QS_ALLINPUT) == WAIT_FAILED)
		goto fail;

	if (!freerdp_check_event_handles(context))
		goto fail;

	bc->bmFormat = context->gdi->drawing->bitmap->format;
	bc->bmWidth = context->gdi->drawing->bitmap->width;
	bc->bmHeight = context->gdi->drawing->bitmap->height;
	bc->bmScanline = context->gdi->drawing->bitmap->scanline;
	bc->bmData = context->gdi->drawing->bitmap->data;

	if (freerdp_shall_disconnect(instance))
		goto fail;

	return *bc;

fail:
	bc->connectionFailure = TRUE;

	return *bc;
}

bridgeContext bridge_get_current_context(void* inst)
{
	freerdp* instance = (freerdp*)inst;
	rdpContext* context;

	context = instance->context;
	bridgeContext* bc = (bridgeContext*)context;

	return *bc;
}

BOOL bridge_freerdp_send_keyboard_event(void* inst, UINT32 scancode, BOOL isDown, BOOL isExtended)
{
	freerdp* instance = (freerdp*)inst;

	if (instance && instance->context && instance->context->input)
	{
		if (isExtended)
		{
			UINT32 scancodeToSend = MAKE_RDP_SCANCODE(scancode, TRUE);
			freerdp_input_send_keyboard_event_ex(instance->context->input, isDown, scancodeToSend);
		}
		else
			freerdp_input_send_keyboard_event_ex(instance->context->input, isDown, scancode);

		return TRUE;
	}
	else
		return FALSE;
}

BOOL bridge_freerdp_send_mouse_event(void* inst, UINT16 flags, UINT16 x, UINT16 y)
{
	freerdp* instance = (freerdp*)inst;

	if (instance && instance->context && instance->context->input)
	{
		freerdp_input_send_mouse_event(instance->context->input, flags, x, y);
		return TRUE;
	}
	else
		return FALSE;
}

BOOL bridge_freerdp_connection_possible(char* hostname, char* domain, int port, char* username, char* password)
{
	WSADATA dummyWsaData;

	int wsaResult = WSAStartup(0x101, &dummyWsaData);

	if (wsaResult != NO_ERROR)
		return FALSE;

	rdpSettings* bridgeRdpSettings = freerdp_settings_new(0);
	freerdp* bridgeRdpInstance = freerdp_new();

	if (!bridgeRdpInstance)
		goto fail;

	bridgeRdpInstance->settings = bridgeRdpSettings;
	bridgeRdpInstance->settings->IgnoreCertificate = TRUE;
	bridgeRdpInstance->settings->ServerHostname = _strdup(hostname);
	bridgeRdpInstance->settings->ServerPort = port;
	bridgeRdpInstance->settings->Username = _strdup(username);
	bridgeRdpInstance->settings->Domain = _strdup(domain);
	bridgeRdpInstance->settings->Password = _strdup(password);
	bridgeRdpInstance->settings->AsyncUpdate = TRUE;
	bridgeRdpInstance->settings->AsyncInput = TRUE;

	bridgeRdpInstance->ContextSize = sizeof(bridgeContext);

	if (!freerdp_context_new(bridgeRdpInstance) || !freerdp_connect(bridgeRdpInstance))
		goto fail;

	// connection succeeded, so close it and free memory
	freerdp_disconnect(bridgeRdpInstance);
	freerdp_context_free(bridgeRdpInstance);
	freerdp_free(bridgeRdpInstance);

	WSACleanup();

	return TRUE;

fail:
	if (bridgeRdpInstance)
	{
		freerdp_context_free(bridgeRdpInstance);
		freerdp_free(bridgeRdpInstance);
	}

	WSACleanup();

	return FALSE;
}

// .NET calls this to clean up an instance
BOOL bridge_cleanup(void* inst, BOOL shouldWSACleanup)
{
	freerdp* instance;

	instance = (freerdp*)inst;

	if (instance)
	{
		freerdp_context_free(instance);
		freerdp_free(instance);
	}

	if (shouldWSACleanup)
		WSACleanup(); // this doesn't appear to do anything

	return TRUE;
}

// TODO: Testing creating and destroying two RDP connections -- when the first connection ends, the second connection
//    should continue without issue

//int main(void)
//{
//	HANDLE handles1[64];
//	DWORD nCount1;
//
//	HANDLE handles2[64];
//	DWORD nCount2;
//
//	char* hostname1 = "vwin2k16.elsitech.local";
//	char* hostname2 = "vwin2k8.elsitech.local";
//	char* domain = "elsitech";
//	char* user = "administrator";
//	char* password = "cheesehead";
//
//	BOOL possible = bridge_freerdp_connection_possible(hostname, domain, 3389, user, password);
//	freerdp* instance1 = initiate_bridge_connect(hostname1, 3389, domain, user, password, 0, 0);
//	rdpContext* context1 = instance1->context;
//	rdpSettings* settings1 = instance1->settings;
//
//	freerdp* instance2 = initiate_bridge_connect(hostname2, 3389, domain, user, password, 0, 0);
//	rdpContext* context2 = instance2->context;
//	rdpSettings* settings2 = instance2->settings;
//
//	bridgeContext* bc1 = (bridgeContext*)context1;
//	bridgeContext* bc2 = (bridgeContext*)context2;
//
//	BOOL instance1Active = TRUE;
//	BOOL instance2Active = TRUE;
//
//	while (1)
//	{
//		DWORD tmp1;
//		DWORD tmp2;
//
//		bc1->updatesReceived = FALSE;
//		bc2->updatesReceived = FALSE;
//
//		nCount1 = 0;
//		nCount2 = 0;
//
//		if (instance1Active)
//			tmp1 = freerdp_get_event_handles(context1, &handles1[nCount1], 64 - nCount1);
//
//		if (instance2Active)
//			tmp2 = freerdp_get_event_handles(context2, &handles2[nCount2], 64 - nCount2);
//
//		if (instance1Active && tmp1 == 0)
//			break;
//
//		if (instance2Active && tmp2 == 0)
//			break;
//
//		if (instance1Active)
//			nCount1 += tmp1;
//
//		if (instance2Active)
//			nCount2 += tmp2;
//
//		if (instance1Active && MsgWaitForMultipleObjects(nCount1, handles1, FALSE, 1000, QS_ALLINPUT) == WAIT_FAILED)
//			break;
//
//		if (instance2Active && MsgWaitForMultipleObjects(nCount2, handles2, FALSE, 1000, QS_ALLINPUT) == WAIT_FAILED)
//			break;
//
//		if (instance1Active && !freerdp_check_event_handles(context1))
//			break;
//
//		if (instance2Active && !freerdp_check_event_handles(context2))
//			break;
//
//		if (instance1Active && freerdp_shall_disconnect(instance1)) // 2k16
//		{
//			bridge_cleanup(instance1, FALSE);
//			instance1Active = FALSE;
//		}
//
//		if (instance2Active && freerdp_shall_disconnect(instance2))
//		{
//			bridge_cleanup(instance2, TRUE);
//			instance2Active = FALSE;
//			break;
//		}
//	}
//
//	return 0;
//}

freerdp* initiate_bridge_connect(char* hostname, int port, char* domain, char* username, char* password, UINT32 desktopWidth, UINT32 desktopHeight)
{
	WSADATA dummyWsaData;

	int wsaResult = WSAStartup(0x101, &dummyWsaData);

	if (wsaResult != NO_ERROR)
		return NULL;

	rdpSettings* bridgeRdpSettings = freerdp_settings_new(0);
	freerdp* bridgeRdpInstance = freerdp_new();

	if (!bridgeRdpInstance)
		goto fail;

	bridgeRdpInstance->settings = bridgeRdpSettings;
	bridgeRdpInstance->settings->IgnoreCertificate = TRUE;
	bridgeRdpInstance->settings->ServerHostname = _strdup(hostname);
	bridgeRdpInstance->settings->ServerPort = port;
	bridgeRdpInstance->settings->Username = _strdup(username);
	bridgeRdpInstance->settings->Domain = _strdup(domain);
	bridgeRdpInstance->settings->Password = _strdup(password);

	// freerdp threw stack overflow when this was set to true
	bridgeRdpInstance->settings->OrderSupport[NEG_MEM3BLT_INDEX] = FALSE;

	if (desktopWidth > 0)
		bridgeRdpInstance->settings->DesktopWidth = desktopWidth;

	if (desktopHeight > 0)
		bridgeRdpInstance->settings->DesktopHeight = desktopHeight;

	bridgeRdpInstance->settings->AsyncInput = TRUE;
	bridgeRdpInstance->settings->AsyncUpdate = TRUE;

	// Set ContextSize on bridgeRdpInstance to sizeof(bridgeContext) before calling freerdp_context_new
	//  this will allocate memory for bridgeContext, which is a superset of plain jane rdpContext
	bridgeRdpInstance->ContextSize = sizeof(bridgeContext);
	bridgeRdpInstance->settings->ColorDepth = 32;

	if (!freerdp_context_new(bridgeRdpInstance) || !freerdp_connect(bridgeRdpInstance))
		goto fail;

	const UINT32 format = PIXEL_FORMAT_BGRX32;
	rdpContext* bridgeRdpContext = bridgeRdpInstance->context;

	// we can pass a NULL buffer into this thing....call to gdi_init_primary will set gdi->primary->bitmap via gdi_CreateCompatibleBitmap
	if (!gdi_init_ex(bridgeRdpInstance, format, 0, NULL, NULL))
		goto fail;

	// Hold off on cursor capturing and translation until rev 2
	//pointer_cache_register_callbacks(bridgeRdpInstance->update);
	//if (!bridge_register_pointer(bridgeRdpInstance->context->graphics))
	//	goto fail;

	bridgeRdpContext->update->BeginPaint = bridge_begin_paint;
	bridgeRdpContext->update->EndPaint = bridge_end_paint;
	bitmap_cache_register_callbacks(bridgeRdpContext->update);

	// bridge context should have reference to the current bitmap
	bridgeContext* bc = (bridgeContext*)bridgeRdpContext;
	bc->connectionFailure = FALSE;

	// .NET will repeatedly pass the instance back to an update method
	return bridgeRdpInstance;

fail:
	freerdp_context_free(bridgeRdpInstance);
	freerdp_free(bridgeRdpInstance);

	WSACleanup();

	return NULL;
}

// Cursor capturing will be a rev 2 feature
static BOOL bridge_register_pointer(rdpGraphics* graphics)
{
	rdpPointer pointer;

	if (!graphics)
		return FALSE;

	ZeroMemory(&pointer, sizeof(rdpPointer));
	pointer.size = sizeof(rdpPointer);
	pointer.New = bridge_pointer_new;
	pointer.Free = bridge_pointer_free;
	pointer.Set = bridge_pointer_set;
	pointer.SetNull = bridge_pointer_set_null;
	pointer.SetDefault = bridge_pointer_set_default;
	pointer.SetPosition = bridge_pointer_set_position;

	graphics_register_pointer(graphics, &pointer);
	return TRUE;
}

// Cursor capturing will be a rev 2 feature
// with this, we want to update the bridgeContext with new ICONINFO info data to be processed as a custom cursor in CursorCapturer
static BOOL bridge_pointer_new(rdpContext* context, const rdpPointer* pointer)
{
	ICONINFO info;
	rdpGdi* gdi;
	BOOL rc = FALSE;

	if (!context || !pointer)
		return FALSE;

	gdi = context->gdi;

	if (!gdi)
		return FALSE;

	info.fIcon = FALSE;
	info.xHotspot = pointer->xPos;
	info.yHotspot = pointer->yPos;

	if (pointer->xorBpp == 1)
	{
		BYTE* pdata = (BYTE*)_aligned_malloc(pointer->lengthAndMask + pointer->lengthXorMask, 16);

		if (!pdata)
			goto fail;

		CopyMemory(pdata, pointer->andMaskData, pointer->lengthAndMask);
		CopyMemory(pdata + pointer->lengthAndMask, pointer->xorMaskData, pointer->lengthXorMask);
		info.hbmMask = CreateBitmap(pointer->width, pointer->height * 2, 1, 1, pdata);
		_aligned_free(pdata);
		info.hbmColor = NULL;
	}
	else
	{
		BYTE* pdata = (BYTE*)_aligned_malloc(pointer->lengthAndMask, 16);

		if (!pdata)
			goto fail;

		flip_bitmap(pointer->andMaskData, pdata, (pointer->width + 7) / 8, pointer->height);
		info.hbmMask = CreateBitmap(pointer->width, pointer->height, 1, 1, pdata);
		_aligned_free(pdata);
		pdata = (BYTE*)_aligned_malloc(pointer->width * pointer->height * GetBitsPerPixel(gdi->dstFormat), 16);

		if (!pdata)
			goto fail;

		if (!freerdp_image_copy_from_pointer_data(pdata, gdi->dstFormat, 0, 0, 0, 
			pointer->width, pointer->height,
			pointer->xorMaskData, pointer->lengthXorMask, 
			pointer->andMaskData, pointer->lengthAndMask, pointer->xorBpp, &gdi->palette))
		{
			_aligned_free(pdata);
			goto fail;
		}

		info.hbmColor = CreateBitmap(pointer->width, pointer->height, 1, GetBitsPerPixel(gdi->dstFormat), pdata);
		_aligned_free(pdata);
	}

	bridgeContext* bc = (bridgeContext*)context;

	//bc->fIcon = info.fIcon;
	//bc->xHotspot = info.xHotspot;
	//bc->yHotspot = info.yHotspot;
	//bc->hbmMask = info.hbmMask;
	//bc->hbmColor = info.hbmColor;

	return TRUE;
fail:

	if (info.hbmMask)
		DeleteObject(info.hbmMask);

	if (info.hbmColor)
		DeleteObject(info.hbmColor);

	return rc;
}

static BOOL bridge_pointer_free(rdpContext* context, rdpPointer* pointer)
{
	return TRUE;
}

static BOOL bridge_pointer_set(rdpContext* context, const rdpPointer* pointer)
{
	return TRUE;
}

static BOOL bridge_pointer_set_null(rdpContext* context)
{
	if (!context)
		return FALSE;

	return TRUE;
}

static BOOL bridge_pointer_set_default(rdpContext* context)
{
	if (!context)
		return FALSE;

	return TRUE;
}

static BOOL bridge_pointer_set_position(rdpContext* context, UINT32 x, UINT32 y)
{
	if (!context)
		return FALSE;

	bridgeContext* bc = (bridgeContext*)context;
	//bc->cursorXpos = x;
	//bc->cursorYpos = y;

	return TRUE;
}

static BOOL flip_bitmap(const BYTE* src, BYTE* dst, UINT32 scanline, UINT32 nHeight)
{
	UINT32 x;
	BYTE* bottomLine = dst + scanline * (nHeight - 1);

	for (x = 0; x < nHeight; x++)
	{
		memcpy(bottomLine, src, scanline);
		src += scanline;
		bottomLine -= scanline;
	}

	return TRUE;
}

static BOOL bridge_begin_paint(rdpContext* context)
{
	HGDI_DC hdc;
	bridgeContext* bc = (bridgeContext*)context;

	if (!context || !context->gdi || !context->gdi->primary || !context->gdi->primary->hdc)
		return FALSE;

	hdc = context->gdi->primary->hdc;

	if (!hdc || !hdc->hwnd || !hdc->hwnd->invalid)
		return FALSE;

	hdc->hwnd->invalid->null = TRUE;
	hdc->hwnd->ninvalid = 0;

	if (!bc->updatesReceived)
		bc->updatesReceived = TRUE;

	return TRUE;
}

static BOOL bridge_end_paint(rdpContext* context)
{
	int i;
	rdpGdi* gdi;
	int ninvalid;
	RECT updateRect;
	HGDI_RGN cinvalid;
	REGION16 invalidRegion;
	RECTANGLE_16 invalidRect;
	const RECTANGLE_16* extents;
	gdi = context->gdi;
	ninvalid = gdi->primary->hdc->hwnd->ninvalid;
	cinvalid = gdi->primary->hdc->hwnd->cinvalid;

	if (ninvalid < 1)
		return TRUE;

	region16_init(&invalidRegion);

	for (i = 0; i < ninvalid; i++)
	{
		invalidRect.left = cinvalid[i].x;
		invalidRect.top = cinvalid[i].y;
		invalidRect.right = cinvalid[i].x + cinvalid[i].w;
		invalidRect.bottom = cinvalid[i].y + cinvalid[i].h;
		region16_union_rect(&invalidRegion, &invalidRegion, &invalidRect);
	}

	if (!region16_is_empty(&invalidRegion))
	{
		extents = region16_extents(&invalidRegion);
		updateRect.left = extents->left;
		updateRect.top = extents->top;
		updateRect.right = extents->right;
		updateRect.bottom = extents->bottom;
		InvalidateRect(context->gdi->hdc->hwnd, &updateRect, FALSE);
	}

	region16_uninit(&invalidRegion);
	return TRUE;
}