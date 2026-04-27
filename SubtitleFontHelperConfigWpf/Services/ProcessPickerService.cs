using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace SubtitleFontHelperConfigWpf.Services;

public sealed class ProcessPickerService
{
    public string? PickProcessExecutableName(Window owner)
    {
        var picker = new NativeOverlayPicker(owner);
        return picker.Pick();
    }

    private sealed class NativeOverlayPicker
    {
        private static readonly object s_classRegistrationLock = new();
        private static readonly NativeMethods.WndProc s_windowProc = StaticOverlayWindowProc;
        private static bool s_overlayClassRegistered;

        private const int VirtualScreenMetricsX = 76;
        private const int VirtualScreenMetricsY = 77;
        private const int VirtualScreenMetricsWidth = 78;
        private const int VirtualScreenMetricsHeight = 79;
        private const int WindowMessageNcCreate = 0x0081;
        private const int WindowMessageNcDestroy = 0x0082;
        private const int WindowMessageMouseMove = 0x0200;
        private const int WindowMessageLeftButtonDown = 0x0201;
        private const int WindowMessageRightButtonDown = 0x0204;
        private const int WindowMessageKeyDown = 0x0100;
        private const int WindowMessageSetCursor = 0x0020;
        private const int VirtualKeyEscape = 0x1B;
        private const int CursorCross = 32515;
        private const int WindowStylePopup = unchecked((int)0x80000000);
        private const int WindowExStyleLayered = 0x00080000;
        private const int WindowExStyleToolWindow = 0x00000080;
        private const int WindowExStyleTopMost = 0x00000008;
        private const uint LayeredWindowAlpha = 0x00000002;
        private const uint AncestorRoot = 2;
        private const int WindowLongUserData = -21;
        private const int GetWindowStyle = -16;
        private const int GetWindowExStyle = -20;
        private const uint WindowStyleVisible = 0x10000000;
        private const uint WindowExStyleTransparent = 0x00000020;
        private const int BlendOperationSourceOver = 0x00;
        private const int AlphaFormatPerPixel = 0x01;
        private const int DibRgbColors = 0;
        private const int BitmapCompressionRgb = 0;
        private const int HolePixel = unchecked((int)0x01000000);
        private const int MaskAlpha = 110;
        private const int BorderOn = unchecked((int)0xFFFFFFFF);
        private const int BorderOff = unchecked((int)0xFF202020);
        private const int BorderDashSize = 6;
        private const string OverlayWindowClassName = "SubtitleFontHelperConfigWpfPickOverlay";

        private readonly Window m_owner;
        private readonly IntPtr m_ownerHandle;
        private readonly IntPtr m_ownerRootHandle;

        private IntPtr m_overlayHandle;
        private IntPtr m_currentTarget;
        private string? m_result;
        private bool m_cancelled;
        private Exception? m_pendingException;
        private NativeMethods.EnumWindowsProc? m_enumWindowsProc;

        public NativeOverlayPicker(Window owner)
        {
            m_owner = owner;
            m_ownerHandle = new WindowInteropHelper(owner).Handle;
            m_ownerRootHandle = NativeMethods.GetAncestor(m_ownerHandle, AncestorRoot);
            m_enumWindowsProc = EnumWindowsProc;
        }

        public string? Pick()
        {
            bool ownerWasVisible = m_owner.IsVisible;
            WindowState ownerWindowState = m_owner.WindowState;

            RegisterOverlayClass();
            try
            {
                HideOwnerWindow();
                CreateOverlayWindow();
                NativeMethods.ShowWindow(m_overlayHandle, 5);
                NativeMethods.SetForegroundWindow(m_overlayHandle);
                UpdateHoverTarget();
                RedrawOverlay();

                System.Windows.Threading.DispatcherFrame frame = new();
                while (frame.Continue && NativeMethods.IsWindow(m_overlayHandle))
                {
                    NativeMethods.Msg message;
                    while (NativeMethods.PeekMessage(out message, IntPtr.Zero, 0, 0, 1))
                    {
                        NativeMethods.TranslateMessage(ref message);
                        NativeMethods.DispatchMessage(ref message);
                    }

                    if (!NativeMethods.IsWindow(m_overlayHandle))
                    {
                        frame.Continue = false;
                        break;
                    }

                    System.Windows.Threading.Dispatcher.CurrentDispatcher.Invoke(
                        () =>
                        {
                            if (!NativeMethods.IsWindow(m_overlayHandle))
                            {
                                frame.Continue = false;
                            }
                        },
                        System.Windows.Threading.DispatcherPriority.Background);
                }
            }
            finally
            {
                if (NativeMethods.IsWindow(m_overlayHandle))
                {
                    NativeMethods.DestroyWindow(m_overlayHandle);
                }

                RestoreOwnerWindow(ownerWasVisible, ownerWindowState);
            }

            if (m_pendingException is not null)
            {
                ExceptionDispatchInfo.Capture(m_pendingException).Throw();
            }

            if (m_cancelled)
            {
                return null;
            }

            return m_result;
        }

        private void HideOwnerWindow()
        {
            if (m_owner.IsVisible)
            {
                m_owner.Hide();
            }
        }

        private void RestoreOwnerWindow(bool ownerWasVisible, WindowState ownerWindowState)
        {
            if (!ownerWasVisible)
            {
                return;
            }

            if (!m_owner.IsVisible)
            {
                m_owner.Show();
            }

            m_owner.WindowState = ownerWindowState;
            m_owner.Activate();
            NativeMethods.SetForegroundWindow(m_ownerHandle);
        }

        private void RegisterOverlayClass()
        {
            lock (s_classRegistrationLock)
            {
                if (s_overlayClassRegistered)
                {
                    return;
                }

                NativeMethods.WndClass windowClass = new()
                {
                    lpfnWndProc = Marshal.GetFunctionPointerForDelegate(s_windowProc),
                    hInstance = NativeMethods.GetModuleHandle(null),
                    hCursor = NativeMethods.LoadCursor(IntPtr.Zero, new IntPtr(CursorCross)),
                    lpszClassName = OverlayWindowClassName,
                };

                ushort atom = NativeMethods.RegisterClass(ref windowClass);
                if (atom == 0)
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error != 1410)
                    {
                        throw new Win32Exception(error);
                    }
                }

                s_overlayClassRegistered = true;
            }
        }

        private void CreateOverlayWindow()
        {
            int x = NativeMethods.GetSystemMetrics(VirtualScreenMetricsX);
            int y = NativeMethods.GetSystemMetrics(VirtualScreenMetricsY);
            int width = NativeMethods.GetSystemMetrics(VirtualScreenMetricsWidth);
            int height = NativeMethods.GetSystemMetrics(VirtualScreenMetricsHeight);

            GCHandle selfHandle = GCHandle.Alloc(this);
            try
            {
                m_overlayHandle = NativeMethods.CreateWindowEx(
                WindowExStyleLayered | WindowExStyleToolWindow | WindowExStyleTopMost,
                OverlayWindowClassName,
                null,
                WindowStylePopup,
                x,
                y,
                width,
                height,
                IntPtr.Zero,
                IntPtr.Zero,
                NativeMethods.GetModuleHandle(null),
                GCHandle.ToIntPtr(selfHandle));

                if (m_overlayHandle == IntPtr.Zero)
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
            }
            catch
            {
                if (selfHandle.IsAllocated)
                {
                    selfHandle.Free();
                }

                throw;
            }
        }

        private static IntPtr StaticOverlayWindowProc(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam)
        {
            try
            {
                if (message == WindowMessageNcCreate)
                {
                    NativeMethods.CreateStruct createStruct = Marshal.PtrToStructure<NativeMethods.CreateStruct>(lParam);
                    NativeMethods.SetWindowLongPtr(hWnd, WindowLongUserData, createStruct.lpCreateParams);
                }

                IntPtr handlePtr = NativeMethods.GetWindowLongPtr(hWnd, WindowLongUserData);
                if (handlePtr == IntPtr.Zero)
                {
                    return NativeMethods.DefWindowProc(hWnd, message, wParam, lParam);
                }

                GCHandle handle = GCHandle.FromIntPtr(handlePtr);
                if (!handle.IsAllocated || handle.Target is not NativeOverlayPicker picker)
                {
                    return NativeMethods.DefWindowProc(hWnd, message, wParam, lParam);
                }

                return picker.HandleWindowMessage(hWnd, message, wParam, lParam);
            }
            catch
            {
                return NativeMethods.DefWindowProc(hWnd, message, wParam, lParam);
            }
        }

        private IntPtr HandleWindowMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam)
        {
            try
            {
                switch (message)
                {
                    case WindowMessageSetCursor:
                        NativeMethods.SetCursor(NativeMethods.LoadCursor(IntPtr.Zero, new IntPtr(CursorCross)));
                        return new IntPtr(1);

                    case WindowMessageMouseMove:
                        UpdateHoverTarget();
                        RedrawOverlay();
                        return IntPtr.Zero;

                    case WindowMessageLeftButtonDown:
                        CompleteSelection();
                        return IntPtr.Zero;

                    case WindowMessageRightButtonDown:
                        CancelSelection();
                        return IntPtr.Zero;

                    case WindowMessageKeyDown:
                        if (wParam.ToInt32() == VirtualKeyEscape)
                        {
                            CancelSelection();
                            return IntPtr.Zero;
                        }

                        break;

                    case WindowMessageNcDestroy:
                        IntPtr handlePtr = NativeMethods.GetWindowLongPtr(hWnd, WindowLongUserData);
                        if (handlePtr != IntPtr.Zero)
                        {
                            GCHandle handle = GCHandle.FromIntPtr(handlePtr);
                            if (handle.IsAllocated)
                            {
                                handle.Free();
                            }

                            NativeMethods.SetWindowLongPtr(hWnd, WindowLongUserData, IntPtr.Zero);
                        }

                        break;
                }
            }
            catch (Exception ex)
            {
                AbortWithFailure(ex);
                return IntPtr.Zero;
            }

            return NativeMethods.DefWindowProc(hWnd, message, wParam, lParam);
        }

        private void AbortWithFailure(Exception ex)
        {
            m_pendingException ??= ex;
            m_result = null;
            m_cancelled = false;

            try
            {
                CancelOverlay();
            }
            catch
            {
            }
        }

        private void UpdateHoverTarget()
        {
            NativeMethods.GetCursorPos(out NativeMethods.Point cursor);
            IntPtr newTarget = FindTopLevelWindowUnder(cursor);
            if (newTarget != m_currentTarget)
            {
                m_currentTarget = newTarget;
            }
        }

        private IntPtr FindTopLevelWindowUnder(NativeMethods.Point point)
        {
            WindowSearchContext context = new()
            {
                ExcludeOverlay = m_overlayHandle,
                ExcludeOwner = m_ownerRootHandle,
                Point = point,
            };

            NativeMethods.EnumWindows(m_enumWindowsProc!, ref context);
            return context.Result;
        }

        private bool EnumWindowsProc(IntPtr window, ref WindowSearchContext context)
        {
            try
            {
                if (window == context.ExcludeOverlay || window == context.ExcludeOwner)
                {
                    return true;
                }

                if (!NativeMethods.IsWindowVisible(window) || NativeMethods.IsIconic(window))
                {
                    return true;
                }

                uint style = unchecked((uint)NativeMethods.GetWindowLong(window, GetWindowStyle));
                if ((style & WindowStyleVisible) == 0)
                {
                    return true;
                }

                uint exStyle = unchecked((uint)NativeMethods.GetWindowLong(window, GetWindowExStyle));
                if ((exStyle & WindowExStyleTransparent) != 0)
                {
                    return true;
                }

                if (!NativeMethods.GetWindowRect(window, out NativeMethods.Rect rect))
                {
                    return true;
                }

                if (!NativeMethods.PtInRect(ref rect, context.Point))
                {
                    return true;
                }

                context.Result = window;
                return false;
            }
            catch (Exception ex)
            {
                AbortWithFailure(ex);
                return false;
            }
        }

        private void RedrawOverlay()
        {
            if (m_overlayHandle == IntPtr.Zero || !NativeMethods.GetWindowRect(m_overlayHandle, out NativeMethods.Rect overlayRect))
            {
                return;
            }

            int width = overlayRect.Right - overlayRect.Left;
            int height = overlayRect.Bottom - overlayRect.Top;
            if (width <= 0 || height <= 0)
            {
                return;
            }

            IntPtr screenDc = NativeMethods.GetDC(IntPtr.Zero);
            if (screenDc == IntPtr.Zero)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }

            IntPtr memoryDc = NativeMethods.CreateCompatibleDC(screenDc);
            if (memoryDc == IntPtr.Zero)
            {
                NativeMethods.ReleaseDC(IntPtr.Zero, screenDc);
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }

            try
            {
                NativeMethods.BitmapInfo bitmapInfo = new()
                {
                    bmiHeader = new NativeMethods.BitmapInfoHeader
                    {
                        biSize = Marshal.SizeOf<NativeMethods.BitmapInfoHeader>(),
                        biWidth = width,
                        biHeight = -height,
                        biPlanes = 1,
                        biBitCount = 32,
                        biCompression = BitmapCompressionRgb,
                    }
                };

                IntPtr bits;
                IntPtr bitmap = NativeMethods.CreateDIBSection(screenDc, ref bitmapInfo, DibRgbColors, out bits, IntPtr.Zero, 0);
                if (bitmap == IntPtr.Zero || bits == IntPtr.Zero)
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }

                IntPtr oldBitmap = NativeMethods.SelectObject(memoryDc, bitmap);
                if (oldBitmap == IntPtr.Zero)
                {
                    NativeMethods.DeleteObject(bitmap);
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }

                try
                {
                    int totalPixels = width * height;
                    int[] pixels = new int[totalPixels];
                    for (int index = 0; index < pixels.Length; index++)
                    {
                        pixels[index] = MaskAlpha << 24;
                    }

                    bool hasTarget = false;
                    NativeMethods.Rect targetRect = default;
                    if (m_currentTarget != IntPtr.Zero && NativeMethods.GetWindowRect(m_currentTarget, out targetRect))
                    {
                        targetRect.Left -= overlayRect.Left;
                        targetRect.Top -= overlayRect.Top;
                        targetRect.Right -= overlayRect.Left;
                        targetRect.Bottom -= overlayRect.Top;
                        targetRect.Left = Math.Max(0, targetRect.Left);
                        targetRect.Top = Math.Max(0, targetRect.Top);
                        targetRect.Right = Math.Min(width, targetRect.Right);
                        targetRect.Bottom = Math.Min(height, targetRect.Bottom);
                        hasTarget = targetRect.Right > targetRect.Left && targetRect.Bottom > targetRect.Top;
                    }

                    if (hasTarget)
                    {
                        for (int y = targetRect.Top; y < targetRect.Bottom; y++)
                        {
                            for (int x = targetRect.Left; x < targetRect.Right; x++)
                            {
                                pixels[(y * width) + x] = HolePixel;
                            }
                        }

                        DrawDashedBorder(pixels, width, height, targetRect);
                    }

                    Marshal.Copy(pixels, 0, bits, pixels.Length);

                    NativeMethods.Point sourcePoint = new(0, 0);
                    NativeMethods.Point targetPoint = new(overlayRect.Left, overlayRect.Top);
                    NativeMethods.Size size = new(width, height);
                    NativeMethods.BlendFunction blend = new()
                    {
                        BlendOp = BlendOperationSourceOver,
                        SourceConstantAlpha = 255,
                        AlphaFormat = AlphaFormatPerPixel,
                    };

                    bool updated = NativeMethods.UpdateLayeredWindow(
                        m_overlayHandle,
                        screenDc,
                        ref targetPoint,
                        ref size,
                        memoryDc,
                        ref sourcePoint,
                        0,
                        ref blend,
                        LayeredWindowAlpha);

                    if (!updated)
                    {
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    }
                }
                finally
                {
                    NativeMethods.SelectObject(memoryDc, oldBitmap);
                    NativeMethods.DeleteObject(bitmap);
                }
            }
            finally
            {
                NativeMethods.DeleteDC(memoryDc);
                NativeMethods.ReleaseDC(IntPtr.Zero, screenDc);
            }
        }

        private static void DrawDashedBorder(int[] pixels, int width, int height, NativeMethods.Rect rect)
        {
            static void SetPixel(int[] targetPixels, int targetWidth, int targetHeight, int x, int y, int color)
            {
                if (x >= 0 && x < targetWidth && y >= 0 && y < targetHeight)
                {
                    targetPixels[(y * targetWidth) + x] = color;
                }
            }

            for (int side = 0; side < 2; side++)
            {
                int y = side == 0 ? rect.Top : rect.Bottom - 1;
                int innerY = side == 0 ? y + 1 : y - 1;
                for (int x = rect.Left; x < rect.Right; x++)
                {
                    int color = ((x / BorderDashSize) & 1) == 0 ? BorderOn : BorderOff;
                    SetPixel(pixels, width, height, x, y, color);
                    SetPixel(pixels, width, height, x, innerY, color);
                }
            }

            for (int side = 0; side < 2; side++)
            {
                int x = side == 0 ? rect.Left : rect.Right - 1;
                int innerX = side == 0 ? x + 1 : x - 1;
                for (int y = rect.Top; y < rect.Bottom; y++)
                {
                    int color = ((y / BorderDashSize) & 1) == 0 ? BorderOn : BorderOff;
                    SetPixel(pixels, width, height, x, y, color);
                    SetPixel(pixels, width, height, innerX, y, color);
                }
            }
        }

        private void CompleteSelection()
        {
            IntPtr target = m_currentTarget;
            CancelOverlay();
            if (target == IntPtr.Zero)
            {
                m_cancelled = true;
                return;
            }

            try
            {
                m_result = GetProcessNameFromWindow(target);
            }
            catch (Exception ex) when (ex is Win32Exception or InvalidOperationException or ArgumentException)
            {
                m_cancelled = true;
                m_result = null;
            }
        }

        private void CancelSelection()
        {
            m_cancelled = true;
            CancelOverlay();
        }

        private void CancelOverlay()
        {
            if (m_overlayHandle != IntPtr.Zero && NativeMethods.IsWindow(m_overlayHandle))
            {
                IntPtr window = m_overlayHandle;
                m_overlayHandle = IntPtr.Zero;
                NativeMethods.DestroyWindow(window);
            }

            NativeMethods.SetForegroundWindow(m_ownerHandle);
        }

        private static string? GetProcessNameFromWindow(IntPtr window)
        {
            NativeMethods.GetWindowThreadProcessId(window, out uint processId);
            if (processId == 0)
            {
                return null;
            }

            using Process process = Process.GetProcessById((int)processId);
            string fullName = process.MainModule?.FileName ?? string.Empty;
            if (string.IsNullOrWhiteSpace(fullName))
            {
                return null;
            }

            return System.IO.Path.GetFileName(fullName);
        }
    }

    private struct WindowSearchContext
    {
        public IntPtr ExcludeOverlay;
        public IntPtr ExcludeOwner;
        public NativeMethods.Point Point;
        public IntPtr Result;
    }

    private static class NativeMethods
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct Point
        {
            public int X;
            public int Y;

            public Point(int x, int y)
            {
                X = x;
                Y = y;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct Rect
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct Size
        {
            public int cx;
            public int cy;

            public Size(int width, int height)
            {
                cx = width;
                cy = height;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct BlendFunction
        {
            public byte BlendOp;
            public byte BlendFlags;
            public byte SourceConstantAlpha;
            public byte AlphaFormat;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct BitmapInfoHeader
        {
            public int biSize;
            public int biWidth;
            public int biHeight;
            public short biPlanes;
            public short biBitCount;
            public int biCompression;
            public int biSizeImage;
            public int biXPelsPerMeter;
            public int biYPelsPerMeter;
            public int biClrUsed;
            public int biClrImportant;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct BitmapInfo
        {
            public BitmapInfoHeader bmiHeader;
            public uint bmiColors;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct WndClass
        {
            public uint style;
            public IntPtr lpfnWndProc;
            public int cbClsExtra;
            public int cbWndExtra;
            public IntPtr hInstance;
            public IntPtr hIcon;
            public IntPtr hCursor;
            public IntPtr hbrBackground;
            public string? lpszMenuName;
            public string lpszClassName;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct Msg
        {
            public IntPtr hwnd;
            public uint message;
            public IntPtr wParam;
            public IntPtr lParam;
            public uint time;
            public Point pt;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct CreateStruct
        {
            public IntPtr lpCreateParams;
            public IntPtr hInstance;
            public IntPtr hMenu;
            public IntPtr hwndParent;
            public int cy;
            public int cx;
            public int y;
            public int x;
            public int style;
            public IntPtr lpszName;
            public IntPtr lpszClass;
            public uint dwExStyle;
        }

        public delegate IntPtr WndProc(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
        public delegate bool EnumWindowsProc(IntPtr hWnd, ref WindowSearchContext context);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern ushort RegisterClass(ref WndClass windowClass);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern IntPtr CreateWindowEx(
            int exStyle,
            string className,
            string? windowName,
            int style,
            int x,
            int y,
            int width,
            int height,
            IntPtr parent,
            IntPtr menu,
            IntPtr instance,
            IntPtr param);

        [DllImport("user32.dll")]
        public static extern bool DestroyWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool IsWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hWnd, int command);

        [DllImport("user32.dll")]
        public static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern IntPtr DefWindowProc(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern bool PeekMessage(out Msg message, IntPtr hWnd, uint minMessage, uint maxMessage, uint removeMessage);

        [DllImport("user32.dll")]
        public static extern bool TranslateMessage(ref Msg message);

        [DllImport("user32.dll")]
        public static extern IntPtr DispatchMessage(ref Msg message);

        [DllImport("user32.dll")]
        public static extern IntPtr LoadCursor(IntPtr instance, IntPtr cursorId);

        [DllImport("user32.dll")]
        public static extern IntPtr SetCursor(IntPtr cursor);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr GetModuleHandle(string? moduleName);

        [DllImport("user32.dll")]
        public static extern int GetSystemMetrics(int index);

        [DllImport("user32.dll")]
        public static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);

        [DllImport("user32.dll")]
        public static extern bool GetCursorPos(out Point point);

        [DllImport("user32.dll")]
        public static extern bool EnumWindows(EnumWindowsProc callback, ref WindowSearchContext context);

        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool IsIconic(IntPtr hWnd);

        [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
        public static extern int GetWindowLong(IntPtr hWnd, int index);

        [DllImport("user32.dll")]
        public static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);

        [DllImport("user32.dll")]
        public static extern bool PtInRect(ref Rect rect, Point point);

        [DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

        [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
        public static extern IntPtr SetWindowLongPtr(IntPtr hWnd, int index, IntPtr newLong);

        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
        public static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int index);

        [DllImport("user32.dll")]
        public static extern IntPtr GetDC(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern int ReleaseDC(IntPtr hWnd, IntPtr dc);

        [DllImport("gdi32.dll")]
        public static extern IntPtr CreateCompatibleDC(IntPtr dc);

        [DllImport("gdi32.dll")]
        public static extern bool DeleteDC(IntPtr dc);

        [DllImport("gdi32.dll")]
        public static extern IntPtr SelectObject(IntPtr dc, IntPtr gdiObject);

        [DllImport("gdi32.dll")]
        public static extern bool DeleteObject(IntPtr objectHandle);

        [DllImport("gdi32.dll")]
        public static extern IntPtr CreateDIBSection(
            IntPtr dc,
            ref BitmapInfo bitmapInfo,
            uint usage,
            out IntPtr bits,
            IntPtr section,
            uint offset);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool UpdateLayeredWindow(
            IntPtr hWnd,
            IntPtr destinationDc,
            ref Point destinationPoint,
            ref Size size,
            IntPtr sourceDc,
            ref Point sourcePoint,
            int colorKey,
            ref BlendFunction blendFunction,
            uint flags);
    }
}
