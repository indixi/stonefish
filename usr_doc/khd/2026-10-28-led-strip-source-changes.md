# Stonefish 灯带功能源码改动说明

记录日期：2026-10-28  
源码位置：`AquaStonefishSim/third_party/stonefish`  
基线提交：`b21eb8e`，Stonefish 1.6.0

本文记录灯带功能在 Stonefish 引擎中的实际改动，便于维护、移植和重新编译。ROS 消息、左右分组、调参面板及闪烁调度的工程说明另见 [ROS 灯带控制说明](../../../../src/doc/2026-10-28-led-strip-control.md)。

## 1. 功能范围与调用关系

在原生 Light 灯珠上增加运行时颜色和光通量控制，继续使用既有球形点光源或聚光灯，不新增灯带实体、碰撞体或水动力模型。

调用关系：

```text
ROS 灯带指令 / dynamic_reconfigure
    → ROSLightController 缓存各组配置
    → 仿真步长推进闪烁时间
    → 渲染线程调用 Light::setLightProperties()
    → OpenGLLight 更新颜色和光强
    → 阴影烘焙、光源 UBO 上传和灯珠绘制
```

灯带的“左右分组、闪烁频率、占空比、相对亮度”由 ROS 层负责。引擎接收的是单个光源的 RGB、绝对光通量和开关状态，不在引擎中实现 ROS 定时器或通信编码。

保留八个现有灯珠的位置、半径及初始场景参数；未修改机器人质量、浮力、PID 或推力分配。

## 2. 文件清单

以下链接相对于本文所在的 `usr_doc/khd` 目录。

| 修改文件 | 主要改动 |
|---|---|
| [ScenarioParser.h](../../Library/include/core/ScenarioParser.h) | ParseColor 从 private 移至 protected，供派生解析器复用 |
| [Light.h](../../Library/include/actuators/Light.h) | 声明动态属性接口，保存光通量换算系数和开关状态 |
| [Light.cpp](../../Library/src/actuators/Light.cpp) | 实现 RGB/光通量更新、输入校验、零亮度恢复及初始化状态恢复 |
| [OpenGLLight.h](../../Library/include/graphics/OpenGLLight.h) | 声明颜色/光强更新接口及活动光源索引接口 |
| [OpenGLLight.cpp](../../Library/src/graphics/OpenGLLight.cpp) | 更新颜色/光强，计算按光源类型压缩的活动索引 |
| [OpenGLSpotLight.h](../../Library/include/graphics/OpenGLSpotLight.h) | 声明聚光灯属性更新和阴影层选择接口，初始化阴影 FBO 状态 |
| [OpenGLSpotLight.cpp](../../Library/src/graphics/OpenGLSpotLight.cpp) | 更新阴影远平面，复用现有 FBO 绑定到活动阴影层 |
| [OpenGLContent.cpp](../../Library/src/graphics/OpenGLContent.cpp) | 发光灯珠绘制改用活动光源索引 |
| [OpenGLPipeline.cpp](../../Library/src/graphics/OpenGLPipeline.cpp) | 按活动聚光灯顺序分配阴影层，阴影烘焙后再上传光源 UBO |
| [CMakeLists.txt](../../CMakeLists.txt) | 增加可选 BUILD_LIGHT_CONTROL_TESTS 构建选项 |

新增测试：

| 文件 | 检查内容 |
|---|---|
| [LightParserAccessTest.cpp](../../Tests/LightParserAccessTest.cpp) | 编译时检查派生类能访问 ParseColor，以及 Light 动态接口签名正确 |
| [LightControlTest.cpp](../../Tests/LightControlTest.cpp) | 颜色、绝对光强、零光强恢复、非法输入及混合光源的活动索引 |

## 3. ParseColor 私有访问编译修复

原 ROS 扩展在 `ROSScenarioParser::ParseLight()` 中复用父类的 `ParseColor()`，但原引擎头文件将其声明在 private 下，因而出现：

```text
error: sf::ScenarioParser::ParseColor(...) is private within this context
```

现在仅将声明移动到 protected：

```cpp
protected:
    bool ParseColor(XMLElement* element, Color& c);
```

`ScenarioParser.cpp` 的解析实现没有变化，继续支持 rgb、hsv、temperature、gray。该调整只允许派生解析器复用，不把解析辅助方法开放为 public。

## 4. Light 动态属性接口

新增接口：

```cpp
void setLightProperties(const Color& color,
                        Scalar luminousPower,
                        bool enabled);
```

| 参数 | 含义 |
|---|---|
| color | RGB 颜色；有限值会约束到 0～1 |
| luminousPower | 绝对光通量，单位 lm；负值归零 |
| enabled | 请求的开关状态；光通量为零时实际光源保持关闭 |

非有限颜色或光通量会被忽略，保留已有状态；转换为渲染浮点数后溢出的光通量或光强也不应用。

ROS 的 brightness 是比例，由 ROS 控制器换算后再传入该接口。例如 SCN 每灯初始值为 100，brightness=0.5 时请求 50 lm；重复提交仍为 50 lm，不会反复乘旧亮度。

新增成员：

```cpp
GLfloat powerToIntensity = 0.f;
bool enabled = true;
```

`InitGraphics()` 使用单位光通量创建原有点光源/聚光灯，读取其构造函数已有的归一化结果作为固定换算系数，再恢复实际光通量。这样复用两种光源各自的换算公式，且不需要除以旧亮度，能够支持 0 → 非零的恢复。

`Switch()` 同时保存开关请求；即使光源尚未初始化，初始化后也能恢复请求状态。亮灯条件为请求开启且光通量大于零。

## 5. OpenGL 颜色与光强更新

新增虚函数：

```cpp
virtual void setColorIntensity(glm::vec3 rgb, GLfloat intensity);
```

此处 intensity 是渲染使用的光强，已经由 Light 层完成光通量换算。该方法只更新 CPU 侧 `colorLi`，不改变活动状态、不重建灯珠网格，也不创建新的 GPU 光源。

现有灯珠发光着色和环境照明继续读取同一份颜色/光强数据，因此控制入口同时作用于灯珠本身及其照明计算。

调用约束：运行时从渲染线程执行。当前 ROS 集成在图形应用的 `ProcessInputs()` 中应用配置，ROS 回调只缓存指令；不能改成在 ROS 回调中直接修改图形对象。

## 6. 独立开关、闪烁与阴影修复

### 6.1 发光灯珠使用活动光源索引

`SetupLights()` 只把开启的光源写入 UBO，并分别压缩点光源和聚光灯的索引。原 `DrawLightSource()` 使用原始数组下标；关闭前面的灯珠后，后面的灯珠就可能读取错误的光源数据。

新增：

```cpp
static GLint getActiveIndex(const std::vector<OpenGLLight*>& lights,
                            size_t lightId);
```

返回目标光源之前“开启且同类型”的光源数量；目标无效或未开启时返回 -1。`DrawLightSource()` 采用该索引，从而支持左灭右亮和左右不同步闪烁。

### 6.2 聚光灯亮度与阴影远平面

`OpenGLSpotLight::setColorIntensity()` 调用基类更新颜色/光强后，根据新光强更新 zFar，并保证：

```cpp
zFar = glm::max(zNear + 1e-3f,
               sqrtf(colorLi.w / MIN_INTENSITY_THRESHOLD));
```

避免亮度为零或很低时产生退化投影，也保证恢复亮度后阴影范围能够恢复。

### 6.3 活动聚光灯与阴影纹理层一致

聚光灯原先在初始化时获得固定阴影层，而光照着色器使用活动聚光灯的压缩索引。前面的聚光灯关闭后，后面的聚光灯可能采样到旧阴影层。

新增 `setShadowmapLayer(GLint layer)`，缓存有效层号。渲染管线按当前开启的聚光灯顺序分配 0、1、2……，烘焙时将已有 FBO 重新绑定到对应纹理层，不在闪烁过程中重新分配 FBO。

### 6.4 阴影矩阵与光源 UBO 同帧更新

将 `content->SetupLights()` 从阴影烘焙之前移至之后。原因是 BakeShadowmap 会更新 clipSpace，亮度变化又会改变 zFar；先烘焙、后上传才能保证 UBO 中的矩阵、远平面与当前阴影纹理一致。

## 7. 构建安装与 ROS 接入

根据本次错误日志，外层 catkin 工作空间是 `/home/yzw/work_khd/AquaStonefishSim`，代码仓库位于其 `src/AquaStonefishSim`。REPO 需要指向同时包含 `src/stonefish_ros` 与 `third_party/stonefish` 的代码仓库。

### 1. 构建并安装补丁版引擎

```bash
REPO=/home/yzw/work_khd/AquaStonefishSim/src/AquaStonefishSim
test -f "$REPO/third_party/stonefish/Library/include/actuators/Light.h"

cmake -S "$REPO/third_party/stonefish" \
      -B "$REPO/third_party/stonefish/build-led" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DBUILD_TESTS=OFF \
      -DBUILD_LIGHT_CONTROL_TESTS=ON
cmake --build "$REPO/third_party/stonefish/build-led" -j2
(cd "$REPO/third_party/stonefish/build-led" && ctest --output-on-failure)
sudo cmake --install "$REPO/third_party/stonefish/build-led"
sudo ldconfig
```

若仓库位于另一位置，只修改 REPO。测试不创建窗口，但仍需要先安装引擎原有的 OpenGL、SDL2、GLM、Freetype 编译依赖。

核对已安装头文件：

```bash
grep -n -B 8 -A 8 'bool ParseColor' /usr/local/include/Stonefish/core/ScenarioParser.h
grep -n 'setLightProperties' /usr/local/include/Stonefish/actuators/Light.h
```

第一条应显示 ParseColor 在 private 标签之前，第二条应显示新增接口。新库增加了类成员和虚函数，必须同步安装头文件与库，并重编依赖，不能只拷贝一个头文件。

### 2. 重新构建 ROS 工程

```bash
source /opt/ros/noetic/setup.bash
cd /home/yzw/work_khd/AquaStonefishSim

catkin build --force-cmake --cmake-args \
  -DStonefish_DIR=/usr/local/lib/cmake/Stonefish

source devel/setup.bash
export LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH:-}"
```

这里使用你当前的 `catkin build`，不需要删除整个 build/devel。若仍显示 private 报错，先核对报错中的实际头文件和上述安装文件是否一致。

仓库现有 `scripts/setup_stonefish_ros.sh` 仍会把旧的 `/home/yzw/work_khd/stonefish/install/lib` 放入 LD_LIBRARY_PATH，并引用旧 catkin_ws 工作空间。本轮未改这个环境脚本；本次请使用上面明确的 setup.bash 路径，并在其他环境脚本执行后再将 /usr/local/lib 放到动态库搜索路径最前面。不要混用旧头文件、旧库与新 ROS 构建。

在外层工作空间中检查运行时库：

```bash
find devel -name parsed_simulator -type f -exec ldd {} \; | grep Stonefish
```

输出应指向补丁版安装库 /usr/local/lib。使用 catkin_tools 隔离 devel 时，可执行文件可能位于隐藏的 .private 子目录，find 会一并搜索。

### 3. 启动与验收

```bash
roslaunch aquaflow_stonefish tracking_urchin.launch \
  scene:=rock_seaurchin vehicle_model:=bricsbot vehicle_name:=bricsbot \
  enable:=true show_led_panel:=true
```

在 rqt_reconfigure 中选择 `/led_strip_controller`，设置左右 mode（0 灭、1 常亮、2 闪烁）、RGB、brightness、blink_hz 和 duty_cycle。每个灯珠原始光通量 100 时，brightness=0.5 请求 50，不会因反复拖动而累计衰减。

```bash
rostopic echo -n 1 /bricsbot/lights/left/cmd/appearance_supported
rostopic echo -n 1 /bricsbot/lights/right/cmd/appearance_supported
```

两项应为 true。依次确认：左绿右蓝初始状态；单独左灭右亮；红绿蓝切换；亮度 1→0.5→0→1；左右不同闪烁频率；暂停冻结相位、恢复继续。本轮不包括可见光通信接收、编码或识别。

## 本地验证记录与边界

- 修复前，真实 ScenarioParser.h 的派生访问检查复现 private 编译错误；灯光测试因缺少新接口而编译失败。
- 修复后，`LightParserAccessTest.cpp` 与实际 `Light.cpp`、`OpenGLLight.cpp`、`OpenGLSpotLight.cpp`、`OpenGLContent.cpp`、`OpenGLPipeline.cpp` 均成功生成目标文件。
- CPU 运行检查通过：直接从修改后的 OpenGLLight.cpp 提取实际 CPU 方法定义，与回归测试一起编译运行，验证颜色/强度更新及活动索引。未执行 GPU 初始化方法。该检查不等于完整库链接测试。
- 阴影调度检查：提取实际旧/新渲染 pass 代码，与无 GPU 的边界替身一起执行。旧版在活动阴影层和矩阵上传时序两项均失败，修正版两项通过。它验证调度逻辑，不代替 GPU 图像验收。
- 现有 Python unittest：5 项通过。
- 现有 ROS 灯带独立 C++ 状态/后端测试通过。
- 引擎与主仓库 diff 检查通过。
- 临时验证使用 Zig C++17、GLM 0.9.9.8 和 SDL2 2.30.11 头文件。针对上游 Windows 宏冲突和平台分支使用了仅在临时目录中的兼容头，没有写入仓库。

本地 Windows 没有 ROS/catkin，也没有完整 Linux 引擎运行环境；完整 OpenGLLight.cpp 单独链接尝试因缺少整套图形/物理库符号未通过，因此改用上述实际 CPU 方法检查。Linux 的完整引擎链接、CTest、catkin build 和画面验收需要按前述步骤执行，不能声称本地已经完成。
