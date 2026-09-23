using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;

namespace GodotPlugins
{
    public class PluginLoadContext : AssemblyLoadContext
    {
        private readonly string _pluginPath;
        private readonly AssemblyDependencyResolver? _resolver;
        private readonly ICollection<string> _sharedAssemblies;
        private readonly AssemblyLoadContext _mainLoadContext;

        public string? AssemblyLoadedPath { get; private set; }

        private static void Log(string message)
        {
            Console.Error.WriteLine(message);
            string formatted = $"[C# PluginLoadContext] {message}{Environment.NewLine}";

            try
            {
                File.AppendAllText("/storage/emulated/0/mono/mono_log.txt", formatted);
            }
            catch { }

            try
            {
                File.AppendAllText("/data/data/org.godotengine.editor.v4.debug/files/mono_log.txt", formatted);
            }
            catch { }
        }

        public PluginLoadContext(string pluginPath, ICollection<string> sharedAssemblies,
            AssemblyLoadContext mainLoadContext, bool isCollectible)
            : base(isCollectible)
        {
            // Auto-resolve kung relative path ang ipinasa (hal. GodotSharp/Tools/GodotTools.dll)
            string resolvedPath = pluginPath;
            if (!Path.IsPathRooted(resolvedPath) || !File.Exists(resolvedPath))
            {
                string fileName = Path.GetFileName(pluginPath);
                string[] searchDirs =
                {
                    "/storage/emulated/0/mono/assemblies",
                    "/storage/emulated/0/mono"
                };

                foreach (string dir in searchDirs)
                {
                    string candidate = Path.Combine(dir, fileName);
                    if (File.Exists(candidate))
                    {
                        resolvedPath = candidate;
                        break;
                    }
                }
            }

            _pluginPath = resolvedPath;
            _sharedAssemblies = sharedAssemblies;
            _mainLoadContext = mainLoadContext;

            Log($"Initializing PluginLoadContext for: {_pluginPath}");

            try
            {
                _resolver = new AssemblyDependencyResolver(_pluginPath);
                Log("AssemblyDependencyResolver initialized successfully.");
            }
            catch (Exception ex)
            {
                Log($"Notice: AssemblyDependencyResolver failed (hostpolicy not active): {ex.Message}. Falling back to manual resolution.");
                _resolver = null;
            }

            if (string.IsNullOrEmpty(AppContext.BaseDirectory))
            {
                string? baseDirectory = Path.GetDirectoryName(_pluginPath);
                if (baseDirectory != null)
                {
                    if (!Path.EndsInDirectorySeparator(baseDirectory))
                        baseDirectory += Path.DirectorySeparatorChar;

                    AppDomain.CurrentDomain.SetData("APP_CONTEXT_BASE_DIRECTORY", baseDirectory);
                    Log($"Set AppContext.BaseDirectory to: {baseDirectory}");
                }
                else
                {
                    Log("Failed to set AppContext.BaseDirectory. Dynamic loading of libraries may fail.");
                }
            }
        }

        protected override Assembly? Load(AssemblyName assemblyName)
        {
            if (assemblyName.Name == null)
                return null;

            Log($"Resolving request for assembly: '{assemblyName.Name}'");

            // 1. Kung shared assembly (GodotSharp, GodotSharpEditor, System core), sa Main Context kunin
            if (_sharedAssemblies.Contains(assemblyName.Name))
            {
                try
                {
                    var sharedAssembly = _mainLoadContext.LoadFromAssemblyName(assemblyName);
                    Log($"Loaded shared assembly from main context: '{assemblyName.Name}'");
                    return sharedAssembly;
                }
                catch (Exception ex)
                {
                    Log($"Notice: Shared assembly '{assemblyName.Name}' not in main context ({ex.Message}). Trying local resolution.");
                }
            }

            // 2. DIRECT CHECK: Kung ito ang mismong project assembly na pinapasa (hal. ForTesting o GodotTools)
            string pluginFileName = Path.GetFileNameWithoutExtension(_pluginPath);
            if (string.Equals(assemblyName.Name, pluginFileName, StringComparison.OrdinalIgnoreCase))
            {
                if (File.Exists(_pluginPath))
                {
                    Log($"Direct hit! Loading target project assembly directly: {_pluginPath}");
                    return LoadAssemblyFromStream(_pluginPath);
                }
            }

            // 3. Subukan ang official resolver kung available
            string? assemblyPath = null;
            if (_resolver != null)
            {
                try
                {
                    assemblyPath = _resolver.ResolveAssemblyToPath(assemblyName);
                }
                catch (Exception ex)
                {
                    Log($"Resolver error on '{assemblyName.Name}': {ex.Message}");
                }
            }

            // 4. FALLBACK: Hanapin sa mismong folder ng project assembly
            if (string.IsNullOrEmpty(assemblyPath))
            {
                string? pluginDir = Path.GetDirectoryName(_pluginPath);
                if (pluginDir != null)
                {
                    string candidate = Path.Combine(pluginDir, assemblyName.Name + ".dll");
                    if (File.Exists(candidate))
                    {
                        assemblyPath = candidate;
                    }
                }
            }

            // 5. FALLBACK: Hanapin sa default Mono assemblies directories
            if (string.IsNullOrEmpty(assemblyPath))
            {
                string[] probeDirs =
                {
                    "/storage/emulated/0/mono/assemblies",
                    "/storage/emulated/0/mono"
                };

                foreach (string dir in probeDirs)
                {
                    if (Directory.Exists(dir))
                    {
                        string candidate = Path.Combine(dir, assemblyName.Name + ".dll");
                        if (File.Exists(candidate))
                        {
                            assemblyPath = candidate;
                            break;
                        }
                    }
                }
            }

            // 6. Kung nahanap ang DLL sa disk, i-load gamit ang stream para maiwasan ang file-locking
            if (!string.IsNullOrEmpty(assemblyPath) && File.Exists(assemblyPath))
            {
                Log($"Resolved '{assemblyName.Name}' to path: {assemblyPath}");
                return LoadAssemblyFromStream(assemblyPath);
            }

            // 7. Huling subok: baka nasa MainLoadContext (TPA)
            try
            {
                var fallbackAss = _mainLoadContext.LoadFromAssemblyName(assemblyName);
                if (fallbackAss != null)
                {
                    Log($"Loaded '{assemblyName.Name}' via fallback to main context.");
                    return fallbackAss;
                }
            }
            catch { }

            Log($"CRITICAL: Unable to resolve assembly '{assemblyName.Name}'.");
            return null;
        }

        private Assembly? LoadAssemblyFromStream(string assemblyPath)
        {
            try
            {
                AssemblyLoadedPath = assemblyPath;

                using var assemblyFile = File.Open(assemblyPath, FileMode.Open, FileAccess.Read, FileShare.Read);
                string pdbPath = Path.ChangeExtension(assemblyPath, ".pdb");

                if (File.Exists(pdbPath))
                {
                    using var pdbFile = File.Open(pdbPath, FileMode.Open, FileAccess.Read, FileShare.Read);
                    var ass = LoadFromStream(assemblyFile, pdbFile);
                    Log($"Loaded into memory with PDB symbols: {assemblyPath}");
                    return ass;
                }

                var loaded = LoadFromStream(assemblyFile);
                Log($"Loaded into memory without PDB: {assemblyPath}");
                return loaded;
            }
            catch (Exception ex)
            {
                Log($"Exception while loading bytes from '{assemblyPath}': {ex}");
                throw;
            }
        }

        protected override IntPtr LoadUnmanagedDll(string unmanagedDllName)
        {
            Log($"Requesting unmanaged DLL: '{unmanagedDllName}'");

            string? libraryPath = null;
            if (_resolver != null)
            {
                try
                {
                    libraryPath = _resolver.ResolveUnmanagedDllToPath(unmanagedDllName);
                }
                catch { }
            }

            if (libraryPath != null && File.Exists(libraryPath))
            {
                Log($"Resolved unmanaged DLL via resolver to: {libraryPath}");
                return LoadUnmanagedDllFromPath(libraryPath);
            }

            // Priority search: unahin ang internal mono_libs kung nasaan ang mga executable .so
            string[] searchDirs =
            {
                "/data/data/org.godotengine.editor.v4.debug/files/mono_libs",
                Path.GetDirectoryName(_pluginPath) ?? "",
                "/storage/emulated/0/mono/assemblies",
                "/storage/emulated/0/mono"
            };

            foreach (string dir in searchDirs)
            {
                if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                    continue;

                string direct = Path.Combine(dir, unmanagedDllName);
                if (File.Exists(direct))
                {
                    Log($"Resolved unmanaged DLL to: {direct}");
                    return LoadUnmanagedDllFromPath(direct);
                }

                if (!unmanagedDllName.EndsWith(".so"))
                {
                    string candidate = Path.Combine(dir, "lib" + unmanagedDllName + ".so");
                    if (File.Exists(candidate))
                    {
                        Log($"Resolved unmanaged DLL to: {candidate}");
                        return LoadUnmanagedDllFromPath(candidate);
                    }
                }
            }

            return IntPtr.Zero;
        }
    }
}
