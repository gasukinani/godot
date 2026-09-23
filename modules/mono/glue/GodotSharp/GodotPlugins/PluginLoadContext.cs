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
            try
            {
                Console.Error.WriteLine(message);
                string logPath = "/storage/emulated/0/mono/mono_log.txt";
                File.AppendAllText(logPath, $"[C# PluginLoadContext] {message}{Environment.NewLine}");
            }
            catch
            {
                // Ignore errors during file logging
            }
        }

        public PluginLoadContext(string pluginPath, ICollection<string> sharedAssemblies,
            AssemblyLoadContext mainLoadContext, bool isCollectible)
            : base(isCollectible)
        {
            _pluginPath = pluginPath;
            _sharedAssemblies = sharedAssemblies;
            _mainLoadContext = mainLoadContext;

            Log($"Initializing PluginLoadContext for: {pluginPath}");

            try
            {
                _resolver = new AssemblyDependencyResolver(pluginPath);
                Log("AssemblyDependencyResolver created successfully.");
            }
            catch (Exception ex)
            {
                Log($"Notice: AssemblyDependencyResolver failed (hostpolicy not loaded): {ex.Message}. Falling back to manual resolution.");
                _resolver = null;
            }

            if (string.IsNullOrEmpty(AppContext.BaseDirectory))
            {
                string? baseDirectory = Path.GetDirectoryName(pluginPath);
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

            Log($"Resolving request for assembly: {assemblyName.Name}");

            // 1. Kung shared assembly (hal. GodotSharp, GodotSharpEditor, CoreLib), gamitin ang Default ALC
            if (_sharedAssemblies.Contains(assemblyName.Name))
            {
                try
                {
                    var sharedAssembly = _mainLoadContext.LoadFromAssemblyName(assemblyName);
                    Log($"Loaded shared assembly from main context: {assemblyName.Name}");
                    return sharedAssembly;
                }
                catch (Exception ex)
                {
                    Log($"Notice: Shared assembly {assemblyName.Name} not in main context ({ex.Message}). Trying local resolution.");
                }
            }

            // 2. DIRECT CHECK: Kung ito ang mismong project assembly na pinapapasa (hal. ForTesting)
            string pluginFileName = Path.GetFileNameWithoutExtension(_pluginPath);
            if (string.Equals(assemblyName.Name, pluginFileName, StringComparison.OrdinalIgnoreCase))
            {
                Log($"Direct hit! Loading target project assembly directly: {_pluginPath}");
                return LoadAssemblyFromStream(_pluginPath);
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
                    Log($"Resolver error on {assemblyName.Name}: {ex.Message}");
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

            // 5. FALLBACK: Hanapin sa karaniwang Android Mono storage directories
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

            // 6. Kung nahanap ang DLL sa disk, i-load sa pamamagitan ng stream (iwas file-locking)
            if (!string.IsNullOrEmpty(assemblyPath) && File.Exists(assemblyPath))
            {
                Log($"Resolved '{assemblyName.Name}' to path: {assemblyPath}");
                return LoadAssemblyFromStream(assemblyPath);
            }

            // 7. Huling subok: baka nasa MainLoadContext (TPA / CoreCLR domain)
            try
            {
                var fallbackAss = _mainLoadContext.LoadFromAssemblyName(assemblyName);
                if (fallbackAss != null)
                {
                    Log($"Loaded '{assemblyName.Name}' via fallback to main context.");
                    return fallbackAss;
                }
            }
            catch
            {
                // Hindi rin nahanap sa main context
            }

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
            Log($"Requesting unmanaged DLL: {unmanagedDllName}");

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
                Log($"Resolved unmanaged DLL to: {libraryPath}");
                return LoadUnmanagedDllFromPath(libraryPath);
            }

            // Local directory check para sa Android native .so
            string? pluginDir = Path.GetDirectoryName(_pluginPath);
            if (pluginDir != null)
            {
                string localLib = Path.Combine(pluginDir, unmanagedDllName);
                if (File.Exists(localLib))
                    return LoadUnmanagedDllFromPath(localLib);

                if (!unmanagedDllName.EndsWith(".so"))
                {
                    localLib = Path.Combine(pluginDir, "lib" + unmanagedDllName + ".so");
                    if (File.Exists(localLib))
                        return LoadUnmanagedDllFromPath(localLib);
                }
            }

            return IntPtr.Zero;
        }
    }
}
