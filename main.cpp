#include <iostream>
#include "DriverRW.h"

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <d3d9.h>
#pragma comment(lib, "d3d9.lib")

#include <d3dx9.h>
#pragma comment(lib, "d3dx9.lib")

using namespace std;

struct vec3
{
	float x, y, z;
};

struct bone_t
{
	BYTE pad[0xCC];
	float x;
	BYTE pad2[0xC];
	float y;
	BYTE pad3[0xC];
	float z;
};

struct viewMatrix_t
{
	float matrix[16];
};

uintptr_t Pid;
uintptr_t BaseAddress;

HWND overlayWindow;
RECT rc;
IDirect3D9Ex* p_Object;
IDirect3DDevice9Ex* p_Device;
D3DPRESENT_PARAMETERS p_Params;
POINT windowWH;
POINT windowXY;

uintptr_t localPlayer;
uintptr_t viewRender;
uintptr_t viewMatrix;
viewMatrix_t vm;

uintptr_t cl_entitylist = 0x1974ad8;
uintptr_t local_player = 0x1d243d8;
uintptr_t view_render = 0x7431238;
uintptr_t view_matrix = 0x5f2891;
constexpr auto m_iName = 0x589;
constexpr auto m_iHealth = 0x438;
constexpr auto m_iTeamNum = 0x448;
constexpr auto m_vecAbsOrigin = 0x14c;
constexpr auto m_Bones = 0xf38;

bool InitWindow()
{
	//获取英伟达叠加层
	overlayWindow = FindWindow(E("CEF-OSC-WIDGET"), E("NVIDIA GeForce Overlay"));
	if (!overlayWindow)
		return false;

	//设置窗口样式
	int i = (int)GetWindowLong(overlayWindow, -20);
	SetWindowLongPtr(overlayWindow, -20, (LONG_PTR)(i | 0x20));
	long style = GetWindowLong(overlayWindow, GWL_EXSTYLE);

	//设置窗口透明度
	MARGINS margin;
	UINT opacity, opacityFlag, colorKey;

	margin.cyBottomHeight = -1;
	margin.cxLeftWidth = -1;
	margin.cxRightWidth = -1;
	margin.cyTopHeight = -1;

	DwmExtendFrameIntoClientArea(overlayWindow, &margin);

	opacityFlag = 0x02;
	colorKey = 0x000000;
	opacity = 0xFF;

	SetLayeredWindowAttributes(overlayWindow, colorKey, opacity, opacityFlag);

	//设置窗口总是在最顶层
	SetWindowPos(overlayWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

	ShowWindow(overlayWindow, SW_SHOW);

	return true;
}

bool DirectXInit()
{
	if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &p_Object)))
		return false;

	GetClientRect(overlayWindow, &rc);

	windowWH = { rc.right - rc.left, rc.bottom - rc.top };
	windowXY = { rc.left, rc.top };

	ZeroMemory(&p_Params, sizeof(p_Params));
	p_Params.Windowed = TRUE;
	p_Params.SwapEffect = D3DSWAPEFFECT_DISCARD;
	p_Params.hDeviceWindow = overlayWindow;
	p_Params.MultiSampleQuality = D3DMULTISAMPLE_NONE;
	p_Params.BackBufferFormat = D3DFMT_A8R8G8B8;
	p_Params.BackBufferWidth = windowWH.x;
	p_Params.BackBufferHeight = windowWH.y;
	p_Params.EnableAutoDepthStencil = TRUE;
	p_Params.AutoDepthStencilFormat = D3DFMT_D16;

	if (FAILED(p_Object->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, overlayWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &p_Params, 0, &p_Device)))
		return false;

	return true;
}

void DrawFilledRectangle(int x, int y, int w, int h, D3DCOLOR color)
{
	D3DRECT rect = { x, y, x + w, y + h };
	p_Device->Clear(1, &rect, D3DCLEAR_TARGET, color, 0, 0);
}

void DrawBorderBox(int x, int y, int w, int h, D3DCOLOR color)
{
	DrawFilledRectangle(x - 1, y - 1, w + 2, 1, color);
	DrawFilledRectangle(x - 1, y, 1, h - 1, color);
	DrawFilledRectangle(x + w, y, 1, h - 1, color);
	DrawFilledRectangle(x - 1, y + h - 1, w + 2, 1, color);
}


bool WorldToScreen(vec3 from, float* m_vMatrix, int targetWidth, int targetHeight, vec3& to)
{
	float w = m_vMatrix[12] * from.x + m_vMatrix[13] * from.y + m_vMatrix[14] * from.z + m_vMatrix[15];

	if (w < 0.01f) return false;

	to.x = m_vMatrix[0] * from.x + m_vMatrix[1] * from.y + m_vMatrix[2] * from.z + m_vMatrix[3];
	to.y = m_vMatrix[4] * from.x + m_vMatrix[5] * from.y + m_vMatrix[6] * from.z + m_vMatrix[7];

	float invw = 1.0f / w;
	to.x *= invw;
	to.y *= invw;

	float x = targetWidth / 2;
	float y = targetHeight / 2;

	x += 0.5 * to.x * targetWidth + 0.5;
	y -= 0.5 * to.y * targetHeight + 0.5;

	to.x = x + windowXY.x;
	to.y = y + windowXY.y;
	to.z = 0;

	return true;
}

vec3 GetBonePos(uintptr_t ent, int id)
{
	vec3 pos = read(ent + m_vecAbsOrigin, vec3);
	uintptr_t bones = read(ent + m_Bones, uintptr_t);
	vec3 bone = {};
	UINT32 boneloc = (id * 0x30);
	bone_t bo = {};
	bo = read(bones + boneloc, bone_t);

	bone.x = bo.x + pos.x;
	bone.y = bo.y + pos.y;
	bone.z = bo.z + pos.z;
	return bone;
}

std::vector<uintptr_t> GetPlayers()
{
	std::vector<uintptr_t> vec;
	for (int i = 0; i <= 100; i++)
	{
		uintptr_t Entity = read((BaseAddress + cl_entitylist) + (i << 5), uintptr_t);
		if (Entity == localPlayer || Entity == 0) continue;

		//检查是否是玩家
		uintptr_t EntityHandle = read(Entity + m_iName, uintptr_t);
		std::string Identifier = read(EntityHandle, std::string);
		LPCSTR IdentifierC = Identifier.c_str();

		if (strcmp(IdentifierC, E("player")))
		{
			vec.push_back(Entity);
		}
	}
	return vec;
}

int main(int argCount, char** argVector)
{
	srand(time(NULL));
	std::string filePath = argVector[0];
	RenameFile(filePath);

	std::cout << E("[+] 正在获取游戏进程") << std::endl;

	while (!get_process_pid(E("r5apex.exe"))) Sleep(1000);

	Pid = get_process_pid(E("r5apex.exe"));

	std::cout << E("[+] Pid：%d") << Pid << std::endl;

	if (!InitWindow())
	{
		std::cout << E("劫持Nvidia叠加层失败") << std::endl;
		system("pause");
		return 0;
	}

	if (!DirectXInit())
	{
		std::cout << E("初始化Nvidia叠加层失败") << std::endl;
		system("pause");
		return 0;
	}

	if (!kernelHandler.attach("r5apex.exe"))
	{
		MessageBox(0, E("附加到游戏进程失败"), E("错误"), MB_OK | MB_ICONERROR);
		return 0;
	}

	//获取基模块地址
	while (BaseAddress == 0)
	{
		BaseAddress = kernelHandler.get_module_base(E("r5apex.exe"));
		Sleep(1000);
	}

	std::cout << E("[+] 模块地址：%p") << BaseAddress << std::endl;

	while (!GetAsyncKeyState(VK_END))
	{
		localPlayer = read(BaseAddress + local_player, uintptr_t);
		viewRender = read(BaseAddress + view_render, uintptr_t);
		viewMatrix = read(viewRender + view_matrix, uintptr_t);
		vm = read(viewMatrix, viewMatrix_t);

		p_Device->Clear(0, 0, D3DCLEAR_TARGET, 0, 1.f, 0);
		p_Device->BeginScene();

		uintptr_t localPlayerHandle = read(localPlayer + m_iName, uintptr_t);
		std::string Identifier = read(localPlayerHandle, std::string);
		LPCSTR IdentifierC = Identifier.c_str();
		if (strcmp(IdentifierC, E("player")))
		{
			for (uintptr_t& player : GetPlayers())
			{
				int health = read(player + m_iHealth, int);
				int teamID = read(player + m_iTeamNum, int);

				if (health < 0 || health > 100 || teamID < 0 || teamID > 32) continue;

				vec3 targetHead = GetBonePos(player, 8);
				vec3 targetHeadScreen;
				if (!WorldToScreen(targetHead, vm.matrix, windowWH.x, windowWH.y, targetHeadScreen)) continue;

				if (targetHeadScreen.x > windowXY.x && targetHeadScreen.y > windowXY.y)
				{
					vec3 targetBody = read(player + m_vecAbsOrigin, vec3);
					vec3 targetBodyScreen;
					if (WorldToScreen(targetBody, vm.matrix, windowWH.x, windowWH.y, targetBodyScreen))
					{
						float height = abs(abs(targetHeadScreen.y) - abs(targetBodyScreen.y));
						float width = height / 2.6f;
						float middle = targetBodyScreen.x - (width / 2);
						D3DCOLOR color = D3DCOLOR_ARGB(255, 255, 0, 0);
						if (teamID == read(localPlayer + m_iTeamNum, int))
							color = D3DCOLOR_ARGB(255, 0, 100, 255);

						DrawBorderBox(middle, targetHeadScreen.y, width, height, color);
					}
				}
			}
		}

		p_Device->EndScene();
		p_Device->PresentEx(0, 0, 0, 0, 0);
	}
}