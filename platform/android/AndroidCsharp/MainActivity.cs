using System;
using Android.App;
using Android.Content.PM;
using Android.OS;
using Android.Util;
using Android.Runtime;
using Android.Widget;
using Silk.NET.Windowing;
using Silk.NET.Windowing.Sdl.Android;

using SilkWindow = Silk.NET.Windowing.Window;

namespace com.queendom.godot
{
    [Activity(
        Label = "Queendom Godot Editor",
        MainLauncher = true,
        Theme = "@android:style/Theme.NoTitleBar.Fullscreen",
        ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.KeyboardHidden | ConfigChanges.Density | ConfigChanges.SmallestScreenSize,
        ScreenOrientation = ScreenOrientation.SensorLandscape,
        HardwareAccelerated = true)]
    public class MainActivity : SilkActivity
    {
        private const string TAG = "QueendomGodot";

        protected override void OnCreate(Bundle? savedInstanceState)
        {
            // CRASH GUARD 1: Saluhin ang lahat ng unhandled C# at Java exceptions
            AppDomain.CurrentDomain.UnhandledException += (s, e) =>
            {
                Log.Error(TAG, $"[FATAL C# EXCEPTION]: {e.ExceptionObject}");
            };

            AndroidEnvironment.UnhandledExceptionRaiser += (s, e) =>
            {
                Log.Error(TAG, $"[FATAL ANDROID EXCEPTION]: {e.Exception}");
                e.Handled = true; // Pigilan ang app na mag-force close agad
            };

            base.OnCreate(savedInstanceState);
        }

        protected override void OnRun()
        {
            try
            {
                Log.Info(TAG, "Starting Silk.NET Engine Initialization...");

                // 1. Silk View Options (OpenGL ES / Vulkan compatible)
                var options = ViewOptions.Default;
                options.FramesPerSecond = 60;
                options.UpdatesPerSecond = 60;

                // 2. Ligtas na kunin ang View
                using var view = SilkWindow.GetView(options);

                view.Load += () =>
                {
                    try
                    {
                        Log.Info(TAG, "Silk Surface Loaded! Initializing Godot Nodes & Editor...");
                        // HINDI na natin kailangan i-init ang Mono dahil tumatakbo na ang C# dito natively!
                    }
                    catch (Exception ex)
                    {
                        Log.Error(TAG, $"Error during Load: {ex}");
                    }
                };

                view.Update += (delta) =>
                {
                    try
                    {
                        // Logic Tick
                    }
                    catch (Exception ex)
                    {
                        Log.Error(TAG, $"Error in Update: {ex.Message}");
                    }
                };

                view.Render += (delta) =>
                {
                    try
                    {
                        // Render Tick (Godot frame rendering)
                    }
                    catch (Exception ex)
                    {
                        Log.Error(TAG, $"Error in Render: {ex.Message}");
                    }
                };

                // 3. Simulan ang main loop nang ligtas
                Log.Info(TAG, "Running View Loop...");
                view.Run();
            }
            catch (Exception ex)
            {
                Log.Error(TAG, $"[CRITICAL ONRUN ERROR]: {ex.Message} \n {ex.StackTrace}");
            }
        }
    }
}
