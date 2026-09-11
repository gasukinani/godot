using System;
using System.Runtime.InteropServices;
using Android.App;
using Android.Content.PM;
using Android.OS;
using Android.Widget;
using Silk.NET.Core.Loader;
using Silk.NET.Windowing;
using Silk.NET.Windowing.Sdl.Android;

namespace com.queendom.godot
{
    [Activity(
        Label = "Queendom Godot Silk Editor",
        MainLauncher = true,
        Theme = "@android:style/Theme.NoTitleBar.Fullscreen",
        ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.KeyboardHidden | ConfigChanges.Density,
        ScreenOrientation = ScreenOrientation.SensorLandscape,
        HardwareAccelerated = true)]
    public class MainActivity : SilkActivity
    {
        private IView? _silkView;
        private IntPtr _godotLibHandle = IntPtr.Zero;
        private IntPtr _monoLibHandle = IntPtr.Zero;

        // P/Invoke Delegates para safe at hindi mag-crash kahit magbago ang symbol mapping
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void GodotInitDelegate(IntPtr env, IntPtr clazz, IntPtr activity, bool isEditor);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void GodotStepDelegate(IntPtr env, IntPtr clazz);

        private GodotInitDelegate? _godotInitialize;
        private GodotStepDelegate? _godotStep;

        protected override void OnCreate(Bundle? savedInstanceState)
        {
            base.OnCreate(savedInstanceState);

            // 1. SILK.NET LOADER: Safe na paghahanap ng mga .so files
            try
            {
                var loader = LibraryLoader.GetPlatformDefaultLoader();

                // Hahanapin ng Silk.NET ang tamang path sa loob ng APK libs
                _monoLibHandle = loader.LoadNativeLibrary("monosgen-2.0");
                _godotLibHandle = loader.LoadNativeLibrary("godot_android");

                if (_godotLibHandle != IntPtr.Zero)
                {
                    // I-link ang native functions dynamically
                    IntPtr initPtr = loader.GetProcAddress(_godotLibHandle, "Java_org_godotengine_godot_GodotLib_initialize");
                    IntPtr stepPtr = loader.GetProcAddress(_godotLibHandle, "Java_org_godotengine_godot_GodotLib_step");

                    if (initPtr != IntPtr.Zero)
                        _godotInitialize = Marshal.GetDelegateForFunctionPointer<GodotInitDelegate>(initPtr);

                    if (stepPtr != IntPtr.Zero)
                        _godotStep = Marshal.GetDelegateForFunctionPointer<GodotStepDelegate>(stepPtr);
                }
            }
            catch (Exception ex)
            {
                Toast.MakeText(this, $"Silk Loader Error: {ex.Message}", ToastLength.Long)?.Show();
            }

            // 2. Silk.NET Window/View setup
            var options = ViewOptions.Default;
            options.FramesPerSecond = 60;
            options.UpdatesPerSecond = 60;

            _silkView = Silk.NET.Windowing.Window.GetView(options);

            _silkView.Load += OnEngineLoad;
            _silkView.Render += OnEngineRender;
            _silkView.Closing += OnEngineClosing;

            // Simulan ang Silk View lifecycle
            _silkView.Initialize();
        }

        private void OnEngineLoad()
        {
            // Kapag ready na ang OpenGL / Vulkan Surface mula sa Silk
            IntPtr env = Android.Runtime.JNIEnv.Handle;
            IntPtr clazz = Android.Runtime.JNIEnv.FindClass("com/queendom/godot/MainActivity");

            // I-initialize ang Godot Editor
            _godotInitialize?.Invoke(env, clazz, this.Handle, true);
        }

        private void OnEngineRender(double delta)
        {
            // Patuloy na i-step ang engine loop bawat frame
            IntPtr env = Android.Runtime.JNIEnv.Handle;
            IntPtr clazz = Android.Runtime.JNIEnv.FindClass("com/queendom/godot/MainActivity");

            _godotStep?.Invoke(env, clazz);
        }

        private void OnEngineClosing()
        {
            // Safe cleanup
        }

        protected override void OnDestroy()
        {
            _silkView?.Reset();
            base.OnDestroy();
        }
    }
}
