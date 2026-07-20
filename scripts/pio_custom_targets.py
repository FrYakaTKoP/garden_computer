Import("env")

# These custom targets appear in the PlatformIO sidebar under the current env.
# They run PlatformIO steps sequentially to avoid serial-port contention.

pio_env = env.subst("$PIOENV")
pio_cmd = '"$PROJECT_CORE_DIR/penv/Scripts/platformio.exe" run -e %s -t ' % pio_env


def step(target_name):
    return pio_cmd + target_name

env.AddCustomTarget(
    name="build_all",
    dependencies=None,
    actions=[
        step("buildprog"),
        step("buildfs"),
    ],
    title="Build All (FW + FS)",
    description="Build firmware and LittleFS image",
)

env.AddCustomTarget(
    name="upload_all",
    dependencies=None,
    actions=[
        step("upload"),
        step("uploadfs"),
    ],
    title="Upload All (FW + FS)",
    description="Upload firmware and LittleFS image",
)

env.AddCustomTarget(
    name="build_upload_all",
    dependencies=None,
    actions=[
        step("buildprog"),
        step("buildfs"),
        step("upload"),
        step("uploadfs"),
    ],
    title="Build + Upload All",
    description="Build and upload firmware and LittleFS image",
)
