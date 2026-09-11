using System;
using Android.App;
using Android.Content.PM;
using Android.OS;
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
        protected override void OnRun()
        {
            // 1. Silk.NET View Configuration
            var options = ViewOptions.Default;
            options.FramesPerSecond = 60;
            options.UpdatesPerSecond = 60;

            // 2. Si Silk.NET na ang bahala sa Android Surface, Graphics Context, at Events
            using var view = Window.GetView(options);

            view.Load += () =>
            {
                // Engine / Editor initialization logic dito
            };

            view.Update += (delta) =>
            {
                // Engine update tick
            };

            view.Render += (delta) =>
            {
                // Render frame loop
            };

            // 3. Simulan ang main loop (Awtomatikong pinapatakbo ng Silk)
            view.Run();
        }
    }
}
