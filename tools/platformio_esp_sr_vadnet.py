from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
vadnet_lib = project_dir / "lib" / "esp-sr" / "lib" / "esp32s3" / "libvadnet.a"

env.Append(LIBS=[env.File(str(vadnet_lib))])
