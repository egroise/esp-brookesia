.. _system-toolkit-sec-00:

Toolkit
-------

:link_to_translation:`en:[English]`

ESP-Brookesia Toolkit 提供用于初始化、构建、打包、校验和模拟 ESP-Brookesia 应用包（``.bpk``）的 npm 工具链。

.. _system-toolkit-sec-01:

环境要求
~~~~~~~~~~~~~~~~~~~~~~~~~~

- Node.js 20 或更高版本。
- JavaScript 打包模板需要在应用目录中执行 ``pnpm install`` 安装依赖。

.. _system-toolkit-sec-02:

安装 Toolkit
~~~~~~~~~~~~~~~~~~~~~~~~~~

最终用户可以全局安装已发布的 npm CLI：

.. code-block:: bash

   npm install -g esp-brookesia-toolkit
   brookesia --help

WASM 模拟器 ``@brookesia/simulator-wasm`` 会作为 CLI 依赖自动安装。

.. _system-toolkit-sec-03:

快速开始
~~~~~~~~~~~~~~~~~~~~~~~~~~

创建并运行一个 JavaScript GUI 应用：

.. code-block:: bash

   brookesia init my-app --template js-gui
   cd my-app
   pnpm install
   brookesia doctor
   brookesia build
   brookesia simulate --target system

``brookesia build`` 生成 debug 版 ``.bpk``。如果需要发布版，请先初始化签名材料：

.. code-block:: bash

   brookesia sign init
   brookesia release
   brookesia verify

.. _system-toolkit-sec-04:

应用模板
~~~~~~~~~~~~~~~~~~~~~~~~~~

``brookesia init`` 支持以下模板：

.. list-table:: Toolkit 应用模板
   :header-rows: 1
   :widths: 20 35 45

   * - 模板
     - 用途
     - 说明
   * - ``js-gui``
     - JavaScript GUI 应用
     - 默认模板，包含 GUI 资源
   * - ``js-bundle``
     - JavaScript 打包应用
     - 使用 rspack 或 webpack 风格的 bundler
   * - ``lua-gui``
     - Lua GUI 应用
     - 自包含应用

例如，指定模板和父目录：

.. code-block:: bash

   brookesia init test --template js-gui --dir my-app

.. _system-toolkit-sec-05:

CLI 命令
~~~~~~~~~~~~~~~~~~~~~~~~~~

查看完整命令和选项：

.. code-block:: bash

   brookesia --help
   brookesia <command> --help

常用命令如下：

.. list-table:: brookesia CLI 命令
   :header-rows: 1
   :widths: 25 75

   * - 命令
     - 说明
   * - ``brookesia init <name>``
     - 从模板创建应用目录，可通过 ``--template`` 和 ``--dir`` 指定模板与父目录。
   * - ``brookesia doctor``
     - 检查应用环境、模拟器和 bundler；可通过 ``--profile`` 指定检查配置。
   * - ``brookesia build``
     - 执行开发构建并生成 debug 版 ``.bpk``。
   * - ``brookesia release``
     - 执行发布构建并生成签名的 release 版 ``.bpk``。
   * - ``brookesia sign init``
     - 在 ``sign/`` 中生成签名公钥和私钥。
   * - ``brookesia pack``
     - 使用 ``--source-dir`` 和 ``--output`` 将目录打包成 ``.bpk``，可用 ``--release`` 进行发布签名。
   * - ``brookesia verify``
     - 校验 release 版 ``.bpk`` 的签名，可通过 ``--package`` 指定文件。
   * - ``brookesia simulate``
     - 启动 PC 模拟器，可通过 ``--target`` 选择模拟器类型。
   * - ``brookesia toolchain install``
     - 记录本地模拟器二进制文件和 GUI 工程目录配置。
   * - ``brookesia dev``
     - 启动 Settings GUI 模拟器，不执行应用构建。

.. _system-toolkit-sec-06:

模拟器模式
~~~~~~~~~~~~~~~~~~~~~~~~~~

``brookesia simulate --target system`` 默认在浏览器中启动 WASM 模拟器，并通过本地 HTTP 服务器提供模拟器页面：

.. code-block:: bash

   brookesia simulate --target system
   brookesia simulate --target system --smoke --duration-ms 2000
   brookesia simulate --target system --gui-debug
   brookesia simulate --target system --resolution 1024x600

支持的主要 target 包括：

- ``system``：在 WASM 浏览器模拟器中运行应用包。
- ``runtime``：使用外部 runtime 模拟器进行运行时侧测试。
- ``gui``：启动已配置的 GUI 模拟器工程。
- ``settings``：启动 Settings GUI 模拟器。
- ``both``：同时启动 GUI 和 system 模拟器。

``@brookesia/simulator-wasm`` 从全局安装的 CLI 解析，而不是从应用的 ``node_modules`` 解析。应用目录中的 ``pnpm install`` 只负责安装 bundler 依赖。
