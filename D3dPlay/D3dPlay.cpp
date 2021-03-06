//D3dPlay.cpp: 定义应用程序的入口点。
//

#include "stdafx.h"
#include "D3dPlay.h"

#include<d3d9.h>
#include <process.h>

extern "C"
{
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
}

//D3D9相关参数
static LPDIRECT3DSURFACE9  g_pDirectSurface9 = NULL;
static LPDIRECT3D9		   g_pD3d;
static LPDIRECT3DDEVICE9   g_pD3dDev;
static D3DPRESENT_PARAMETERS g_D3dParam;
static RECT                g_rcWin = { 0 };
static HWND			       g_pPlayWnd = NULL;
static D3DFORMAT           g_D3dFmt;

//ffmpeg相关参数
static AVFormatContext *g_pAvFmtCtx = NULL;
static AVCodecContext  *g_pAvCodecCtx = NULL;
static int              g_VideoIndex = -1;
static AVCodec         *g_pAvCodec = NULL;
static HANDLE           g_hVideoRenderThread = NULL;
static AVPixelFormat    g_AVFix;


#define MAX_LOADSTRING 100

// 全局变量: 
HINSTANCE hInst;                                // 当前实例
WCHAR szTitle[MAX_LOADSTRING];                  // 标题栏文本
WCHAR szWindowClass[MAX_LOADSTRING];            // 主窗口类名

// 此代码模块中包含的函数的前向声明: 
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
static unsigned int CALLBACK VideoRenderThread(LPVOID p);

void Play()
{

	//初始化libavformat并且注册所有的复用和解复用协议
	av_register_all();

	//分配一个AVFormatContext
	g_pAvFmtCtx = avformat_alloc_context();	

	if (avformat_open_input(&g_pAvFmtCtx, "./说散就散.mp4", NULL, NULL) < 0)
	{
		//打开输入文件失败
		return;
	}

	if (avformat_find_stream_info(g_pAvFmtCtx, NULL) < 0)
	{
		//找不到流的相关信息
		return;
	}

	for (unsigned int i = 0; i < g_pAvFmtCtx->nb_streams; i++)
	{
		if (g_pAvFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			g_VideoIndex = i;
		}
	}

	//分配一个AVCodecContext并且给字段填充默认的值
	g_pAvCodecCtx = avcodec_alloc_context3(NULL);

	//填充编解码器上下字段值基于提供的编解码器参数
	if (avcodec_parameters_to_context(g_pAvCodecCtx,
		g_pAvFmtCtx->streams[g_VideoIndex]->codecpar) < 0)
	{
		//填充参数出错
		return;
	}

	//找到一个与编码器Id匹配并且已注册的解码器
	g_pAvCodec = avcodec_find_decoder(g_pAvCodecCtx->codec_id);
	if (g_pAvCodec == NULL)
	{
		//解码器没找到
		return;
	}

	//用给定的编解码器去初始化编解码器上下文参数
	if (avcodec_open2(g_pAvCodecCtx, g_pAvCodec, NULL) < 0)
	{
		//上下文初始化失败
		return;
	}

	//Direct3d COM接口
	g_pDirectSurface9 = (LPDIRECT3DSURFACE9)calloc(1, sizeof(IDirect3DSurface9));
	//创建Direct3D9对象
	g_pD3d = Direct3DCreate9(D3D_SDK_VERSION);

	//描述显示模式的结构体参数
	D3DDISPLAYMODE d3dmode = { 0 };
	ZeroMemory(&g_D3dParam, sizeof(D3DPRESENT_PARAMETERS));
	g_pD3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &d3dmode);
	g_D3dParam.BackBufferFormat = D3DFMT_UNKNOWN;       //缓冲区格式
	g_D3dParam.BackBufferCount = 1;
	g_D3dParam.BackBufferWidth = g_pAvCodecCtx->width;
	g_D3dParam.BackBufferHeight = g_pAvCodecCtx->height;
	g_D3dParam.MultiSampleType = D3DMULTISAMPLE_NONE;
	g_D3dParam.SwapEffect = D3DSWAPEFFECT_DISCARD;
	g_D3dParam.hDeviceWindow = g_pPlayWnd;
	g_D3dParam.Windowed = TRUE;
	g_D3dParam.EnableAutoDepthStencil = FALSE;
	g_D3dParam.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

	//创建一个设置去呈现显示适配器
	if(FAILED(g_pD3d->CreateDevice(D3DADAPTER_DEFAULT
		, D3DDEVTYPE_HAL, g_pPlayWnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING
		, &g_D3dParam, &g_pD3dDev)))
	{
		//D3DERR_DEVICELOST, D3DERR_INVALIDCALL, D3DERR_NOTAVAILABLE, D3DERR_OUTOFVIDEOMEMORY
		return;
	}
	//创建一个屏幕之外的平面
	if (SUCCEEDED(g_pD3dDev->CreateOffscreenPlainSurface(1, 1, D3DFMT_YUY2,
		D3DPOOL_DEFAULT, &g_pDirectSurface9, NULL)))
	{
		g_D3dFmt = D3DFMT_YUY2;
		g_AVFix = AV_PIX_FMT_YUYV422;
	}
	else if (SUCCEEDED(g_pD3dDev->CreateOffscreenPlainSurface(1, 1, D3DFMT_UYVY,
		D3DPOOL_DEFAULT, &g_pDirectSurface9, NULL)))
	{
		g_D3dFmt = D3DFMT_UYVY;
		g_AVFix = AV_PIX_FMT_YUYV422;
	}
	else
	{
		g_D3dFmt = D3DFMT_X8R8G8B8;
		g_AVFix = AV_PIX_FMT_YUYV422;
	}

	g_pDirectSurface9->Release();
	g_pDirectSurface9 = NULL;


	//启线程去做视频渲染，防止主线程窗口卡死
	g_hVideoRenderThread = (HANDLE)_beginthreadex(NULL, 0, VideoRenderThread, NULL, 0, NULL);
}

unsigned int _stdcall VideoRenderThread(LPVOID p)
{
	//分配一个AVPacket并且填充字段默认值
	AVPacket *pkt = av_packet_alloc();

	AVFrame  *pFrame = av_frame_alloc();
	AVFrame  *pFrameYUV = av_frame_alloc();

	RECT rcClient = {0};
	//获取显示窗口的矩形区域

	SwsContext* img_convert_ctx = NULL;

	while (true)
	{
		if (av_read_frame(g_pAvFmtCtx, pkt) == 0)
		{
			if (pkt->stream_index == g_VideoIndex)
			{
				if (avcodec_send_packet(g_pAvCodecCtx, pkt) == 0)
				{
					if (avcodec_receive_frame(g_pAvCodecCtx, pFrame) == 0)
					{
						/*读取一帧成功，显示图像*/

						//防止窗口变化获取窗口大小
						GetClientRect(g_pPlayWnd, &rcClient);
						if (rcClient.left == rcClient.bottom == rcClient.right == rcClient.top == 0
							|| rcClient.left != g_rcWin.left
							|| rcClient.top != g_rcWin.top
							|| rcClient.right != g_rcWin.right
							|| rcClient.bottom != g_rcWin.bottom)
						{
							InvalidateRect(g_pPlayWnd, &rcClient, FALSE);
							g_rcWin = rcClient;
							sws_freeContext(img_convert_ctx);
							img_convert_ctx = sws_getContext(g_pAvCodecCtx->width, g_pAvCodecCtx->height
								, g_pAvCodecCtx->pix_fmt, g_rcWin.right - g_rcWin.left, g_rcWin.bottom - g_rcWin.top
								, g_AVFix, SWS_BICUBIC, NULL, NULL, NULL);
							if (g_pDirectSurface9)
							{
								g_pDirectSurface9->Release();
								g_pDirectSurface9 = NULL;
							}
						}

						if (!g_pDirectSurface9) {
							// create surface
							if (FAILED(g_pD3dDev->CreateOffscreenPlainSurface(g_rcWin.right-g_rcWin.left
								, g_rcWin.bottom-g_rcWin.top, (D3DFORMAT)g_D3dFmt,
								D3DPOOL_DEFAULT, &g_pDirectSurface9, NULL)))
							{
								return 0;
							}
						}

						//锁定纹理矩形区域
						D3DLOCKED_RECT d3d_rect;
						g_pDirectSurface9->LockRect(&d3d_rect, NULL, D3DLOCK_DISCARD);
						pFrameYUV->data[0] = (uint8_t*)d3d_rect.pBits;
						pFrameYUV->linesize[0] = d3d_rect.Pitch;

						sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize
							, 0, g_pAvCodecCtx->height,
							pFrameYUV->data, pFrameYUV->linesize);

						g_pDirectSurface9->UnlockRect();

						IDirect3DSurface9 *pBackBuffer = NULL;
						if (SUCCEEDED(g_pD3dDev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer))) {
							if (pBackBuffer) {
								if (SUCCEEDED(g_pD3dDev->StretchRect(g_pDirectSurface9, NULL, pBackBuffer, NULL, D3DTEXF_LINEAR))) {
									g_pD3dDev->Present(NULL, &g_rcWin, NULL, NULL);
								}
								pBackBuffer->Release();
							}
						}
						Sleep(35);
					}
				}
			}	
		}
	}
	return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: 在此放置代码。

    // 初始化全局字符串
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_D3DPLAY, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // 执行应用程序初始化: 
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_D3DPLAY));

    MSG msg;

	//播放
	Play();
    // 主消息循环: 
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}



//
//  函数: MyRegisterClass()
//
//  目的: 注册窗口类。
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_D3DPLAY));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_D3DPLAY);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   函数: InitInstance(HINSTANCE, int)
//
//   目的: 保存实例句柄并创建主窗口
//
//   注释: 
//
//        在此函数中，我们在全局变量中保存实例句柄并
//        创建和显示主程序窗口。
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // 将实例句柄存储在全局变量中

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   //此处记录一下图像渲染的窗口
   g_pPlayWnd = hWnd;

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

//
//  函数: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  目的:    处理主窗口的消息。
//
//  WM_COMMAND  - 处理应用程序菜单
//  WM_PAINT    - 绘制主窗口
//  WM_DESTROY  - 发送退出消息并返回
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // 分析菜单选择: 
            switch (wmId)
            {
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            // TODO: 在此处添加使用 hdc 的任何绘图代码...
            EndPaint(hWnd, &ps);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// “关于”框的消息处理程序。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
