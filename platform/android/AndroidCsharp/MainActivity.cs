using System;
using Android.App;
using Android.Content.PM;
using Android.OS;
using Silk.NET.Windowing;
using Silk.NET.Windowing.Sdl.Android;

// ALIAS PARA MAIWASAN ANG PANGALANG CONFLICT SA ANDROID.VIEWS.WINDOW
using SilkWindow = Silk.NET.Windowing.Window;

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
        protected override void OnRun()
        {
            // 1. Silk.NET View Configuration
            var options = ViewOptions.Default;
            options.FramesPerSecond = 60;
            options.UpdatesPerSecond = 60;

            // 2. Gamitin ang SilkWindow para diretsong makuha ang Android View
            using var view = SilkWindow.GetView(options);

            view.Load += () =>
            {
                // Engine / Editor initialization
            };

            view.Update += (delta) =>
            {
                // Engine update tick
            };

            view.Render += (delta) =>
            {
                // Frame render loop
            };

            // 3. Patakbuhin ang main loop
            view.Run();
        }
    }
}
