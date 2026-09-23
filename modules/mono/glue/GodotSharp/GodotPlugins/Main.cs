using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using Godot.Bridge;
using Godot.NativeInterop;

namespace GodotPlugins
{
    public static class Main
    {
        public static void Log(string message)
        {
            Console.Error.WriteLine(message);
            string formatted = $"[C# GodotPlugins] {message}{Environment.NewLine}";

            // Subukang magsulat sa external storage
            try
            {
                File.AppendAllText("/storage/emulated/0/mono/mono_log.txt", formatted);
            }
            catch { }

            // Subukang magsulat sa internal app storage (100% permitted kahit sa Android 11-15)
            try
            {
                File.AppendAllText("/data/data/org.godotengine.editor.v4.debug/files/mono_log.txt", formatted);
            }
            catch { }
        }

        private sealed class PluginLoadContextWrapper
        {
            private PluginLoadContext? _pluginLoadContext;
            private readonly WeakReference _weakReference;

            private PluginLoadContextWrapper(PluginLoadContext pluginLoadContext, WeakReference weakReference)
            {
                _pluginLoadContext = pluginLoadContext;
                _weakReference = weakReference;
            }

            public string? AssemblyLoadedPath
            {
                [MethodImpl(MethodImplOptions.NoInlining)]
                get => _pluginLoadContext?.AssemblyLoadedPath;
            }

            public bool IsCollectible
            {
                [MethodImpl(MethodImplOptions.NoInlining)]
                get => _pluginLoadContext?.IsCollectible ?? true;
            }

            public bool IsAlive
            {
                [MethodImpl(MethodImplOptions.NoInlining)]
                get => _weakReference.IsAlive;
            }

            [MethodImpl(MethodImplOptions.NoInlining)]
            public static (Assembly, PluginLoadContextWrapper) CreateAndLoadFromAssemblyName(
                AssemblyName assemblyName,
                string pluginPath,
                ICollection<string> sharedAssemblies,
                AssemblyLoadContext mainLoadContext,
                bool isCollectible
            )
            {
                Log($"Wrapper: Creating PluginLoadContext for '{pluginPath}'");
                var context = new PluginLoadContext(pluginPath, sharedAssemblies, mainLoadContext, isCollectible);
                var reference = new WeakReference(context, trackResurrection: true);
                var wrapper = new PluginLoadContextWrapper(context, reference);

                Log($"Wrapper: Loading assembly by name '{assemblyName.Name}'");
                var assembly = context.LoadFromAssemblyName(assemblyName);

                if (assembly == null)
                {
                    Log($"Wrapper: ERROR - LoadFromAssemblyName returned null for '{assemblyName.Name}'");
                    throw new FileNotFoundException($"Failed to load assembly '{assemblyName.Name}' from '{pluginPath}'.");
                }

                Log($"Wrapper: Successfully loaded assembly '{assembly.FullName}'");
                return (assembly, wrapper);
            }

            [MethodImpl(MethodImplOptions.NoInlining)]
            internal void Unload()
            {
                _pluginLoadContext?.Unload();
                _pluginLoadContext = null;
            }
        }

        private static readonly List<AssemblyName> SharedAssemblies = new();
        private static readonly Assembly CoreApiAssembly = typeof(global::Godot.GodotObject).Assembly;
        private static Assembly? _editorApiAssembly;
        private static PluginLoadContextWrapper? _projectLoadContext;
        private static bool _editorHint = false;

        private static readonly AssemblyLoadContext MainLoadContext =
            AssemblyLoadContext.GetLoadContext(Assembly.GetExecutingAssembly()) ??
            AssemblyLoadContext.Default;

        private static DllImportResolver? _dllImportResolver;

        [UnmanagedCallersOnly]
        private static unsafe godot_bool InitializeFromEngine(IntPtr godotDllHandle, godot_bool editorHint,
            PluginsCallbacks* pluginsCallbacks, ManagedCallbacks* managedCallbacks,
            IntPtr unmanagedCallbacks, int unmanagedCallbacksSize)
        {
            try
            {
                Log("InitializeFromEngine started...");
                _editorHint = editorHint.ToBool();

                _dllImportResolver = new GodotDllImportResolver(godotDllHandle).OnResolveDllImport;

                SharedAssemblies.Add(CoreApiAssembly.GetName());
                NativeLibrary.SetDllImportResolver(CoreApiAssembly, _dllImportResolver);

                AlcReloadCfg.Configure(alcReloadEnabled: _editorHint);
                NativeFuncs.Initialize(unmanagedCallbacks, unmanagedCallbacksSize);

                if (_editorHint)
                {
                    Log("Editor hint enabled. Loading GodotSharpEditor...");
                    _editorApiAssembly = Assembly.Load("GodotSharpEditor");
                    SharedAssemblies.Add(_editorApiAssembly.GetName());
                    NativeLibrary.SetDllImportResolver(_editorApiAssembly, _dllImportResolver);
                }

                *pluginsCallbacks = new()
                {
                    LoadProjectAssemblyCallback = &LoadProjectAssembly,
                    LoadToolsAssemblyCallback = &LoadToolsAssembly,
                    UnloadProjectPluginCallback = &UnloadProjectPlugin,
                };

                *managedCallbacks = ManagedCallbacks.Create();

                Log("InitializeFromEngine completed successfully.");
                return godot_bool.True;
            }
            catch (Exception e)
            {
                Log($"CRITICAL EXCEPTION in InitializeFromEngine: {e}");
                return godot_bool.False;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PluginsCallbacks
        {
            public unsafe delegate* unmanaged<char*, godot_string*, godot_bool> LoadProjectAssemblyCallback;
            public unsafe delegate* unmanaged<char*, IntPtr, int, IntPtr> LoadToolsAssemblyCallback;
            public unsafe delegate* unmanaged<godot_bool> UnloadProjectPluginCallback;
        }

        [UnmanagedCallersOnly]
        private static unsafe godot_bool LoadProjectAssembly(char* nAssemblyPath, godot_string* outLoadedAssemblyPath)
        {
            try
            {
                if (_projectLoadContext != null)
                {
                    Log("Project assembly already loaded in context.");
                    return godot_bool.True;
                }

                string assemblyPath = new(nAssemblyPath);
                Log($"LoadProjectAssembly invoked for: {assemblyPath}");

                if (!File.Exists(assemblyPath))
                {
                    Log($"ERROR: Target assembly does not exist at path: {assemblyPath}");
                    return godot_bool.False;
                }

                Log("Calling LoadPlugin...");
                (var projectAssembly, _projectLoadContext) = LoadPlugin(assemblyPath, isCollectible: _editorHint);

                if (projectAssembly == null)
                {
                    Log("ERROR: projectAssembly is NULL after LoadPlugin.");
                    return godot_bool.False;
                }

                string loadedAssemblyPath = _projectLoadContext.AssemblyLoadedPath ?? assemblyPath;
                Log($"Setting loaded assembly path: {loadedAssemblyPath}");
                *outLoadedAssemblyPath = Marshaling.ConvertStringToNative(loadedAssemblyPath);

                Log("Invoking ScriptManagerBridge.LookupScriptsInAssembly...");
                try
                {
                    ScriptManagerBridge.LookupScriptsInAssembly(projectAssembly);
                    Log("ScriptManagerBridge.LookupScriptsInAssembly completed successfully!");
                }
                catch (ReflectionTypeLoadException rex)
                {
                    Log($"ReflectionTypeLoadException in LookupScriptsInAssembly: {rex.Message}");
                    if (rex.LoaderExceptions != null)
                    {
                        foreach (var le in rex.LoaderExceptions)
                        {
                            if (le != null)
                                Log($"  -> LoaderException: {le.Message}");
                        }
                    }
                    throw;
                }

                Log("FULL SUCCESS! LoadProjectAssembly returning godot_bool.True.");
                return godot_bool.True;
            }
            catch (Exception e)
            {
                Log($"CRITICAL EXCEPTION in LoadProjectAssembly: {e}");
                return godot_bool.False;
            }
        }

        [UnmanagedCallersOnly]
        private static unsafe IntPtr LoadToolsAssembly(char* nAssemblyPath,
            IntPtr unmanagedCallbacks, int unmanagedCallbacksSize)
        {
            try
            {
                string assemblyPath = new(nAssemblyPath);
                Log($"LoadToolsAssembly invoked for: {assemblyPath}");

                if (_editorApiAssembly == null)
                    throw new InvalidOperationException("The Godot editor API assembly is not loaded.");

                var (assembly, _) = LoadPlugin(assemblyPath, isCollectible: false);

                NativeLibrary.SetDllImportResolver(assembly, _dllImportResolver!);

                var method = assembly.GetType("GodotTools.GodotSharpEditor")?
                    .GetMethod("InternalCreateInstance",
                        BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);

                if (method == null)
                {
                    throw new MissingMethodException("GodotTools.GodotSharpEditor",
                        "InternalCreateInstance");
                }

                return (IntPtr?)method
                           .Invoke(null, new object[] { unmanagedCallbacks, unmanagedCallbacksSize })
                       ?? IntPtr.Zero;
            }
            catch (Exception e)
            {
                Log($"CRITICAL EXCEPTION in LoadToolsAssembly: {e}");
                return IntPtr.Zero;
            }
        }

        private static (Assembly, PluginLoadContextWrapper) LoadPlugin(string assemblyPath, bool isCollectible)
        {
            string assemblyName = Path.GetFileNameWithoutExtension(assemblyPath);
            Log($"LoadPlugin: assemblyName='{assemblyName}', isCollectible={isCollectible}");

            var sharedAssemblies = new List<string>();

            foreach (var sharedAssembly in SharedAssemblies)
            {
                string? sharedAssemblyName = sharedAssembly.Name;
                if (sharedAssemblyName != null)
                    sharedAssemblies.Add(sharedAssemblyName);
            }

            Log($"LoadPlugin: Passing {sharedAssemblies.Count} shared assemblies to context.");

            return PluginLoadContextWrapper.CreateAndLoadFromAssemblyName(
                new AssemblyName(assemblyName), assemblyPath, sharedAssemblies, MainLoadContext, isCollectible);
        }

        [UnmanagedCallersOnly]
        private static godot_bool UnloadProjectPlugin()
        {
            try
            {
                return UnloadPlugin(ref _projectLoadContext).ToGodotBool();
            }
            catch (Exception e)
            {
                Log($"CRITICAL EXCEPTION in UnloadProjectPlugin: {e}");
                return godot_bool.False;
            }
        }

        private static bool UnloadPlugin(ref PluginLoadContextWrapper? pluginLoadContext)
        {
            try
            {
                if (pluginLoadContext == null)
                    return true;

                if (!pluginLoadContext.IsCollectible)
                {
                    Log("Cannot unload a non-collectible assembly load context.");
                    return false;
                }

                Log("Unloading assembly load context...");

                pluginLoadContext.Unload();

                int startTimeMs = Environment.TickCount;
                bool takingTooLong = false;

                while (pluginLoadContext.IsAlive)
                {
                    GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced);
                    GC.WaitForPendingFinalizers();

                    if (!pluginLoadContext.IsAlive)
                        break;

                    int elapsedTimeMs = Environment.TickCount - startTimeMs;

                    if (!takingTooLong && elapsedTimeMs >= 200)
                    {
                        takingTooLong = true;
                        Log("Assembly unloading is taking longer than expected...");
                    }
                    else if (elapsedTimeMs >= 1000)
                    {
                        Log("Failed to unload assemblies. Possible causes: Strong GC handles, running threads, etc.");
                        return false;
                    }
                }

                Log("Assembly load context unloaded successfully.");
                pluginLoadContext = null;
                return true;
            }
            catch (Exception e)
            {
                Log($"CRITICAL EXCEPTION in UnloadPlugin: {e}");
                return false;
            }
        }
    }
}
