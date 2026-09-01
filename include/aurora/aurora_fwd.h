#pragma once

/**
 * @file aurora_fwd.h
 * @brief 前向声明头（可选、不破坏现有 API）。
 *
 * 与 `aurora.h`（单一包含入口，拉入全部抽象层）互补：当某翻译单元只需以
 * 指针/引用形式提及下列重量级门面类型、而不需要其完整定义时，可仅包含本头，
 * 以降低单 TU 的瞬时包含成本。需要完整 API 时仍应 `#include "aurora/aurora.h"`。
 *
 * 本头**不**前向声明值类型（Color/Rect/KeyCode/枚举等，按值使用时需完整定义）
 * 与模板（Provider<T>/Signal<T> 等，需完整模板头）；它们由各模块头提供。
 */
namespace aurora {

class Surface;     ///< 抽象渲染后端（Headless/GLFW/Win32/X11/Wayland/...）
class Window;      ///< 窗口宿主（创建/消息/DPI/事件翻译）
class Painter;     ///< 软件栅格绘制内核
class FontEngine;  ///< 字体整形与光栅化引擎
class Widget;      ///< 控件基类（声明式节点）
class Application; ///< 应用生命周期（run/事件循环/资源配置）
class App;         ///< 轻量应用门面
class Navigator;   ///< 导航栈控制器
class Router;      ///< 路由表
struct Theme;      ///< 主题（颜色/排版/形状令牌）

} // namespace aurora
