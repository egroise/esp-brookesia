.. _system-toolkit-sec-00:

Toolkit
-------

:link_to_translation:`zh_CN:[中文]`

ESP-Brookesia Toolkit provides npm tools for initializing, building, packing, verifying, and simulating ESP-Brookesia app packages (``.bpk``).

.. _system-toolkit-sec-01:

Environment Requirements
~~~~~~~~~~~~~~~~~~~~~~~~~~

- Node.js 20 or newer.
- JavaScript bundling templates require ``pnpm install`` in the app directory.

.. _system-toolkit-sec-02:

Install the Toolkit
~~~~~~~~~~~~~~~~~~~~~~~~~~

End users can install the published npm CLI globally:

.. code-block:: bash

   npm install -g esp-brookesia-toolkit
   brookesia --help

The WASM simulator, ``@brookesia/simulator-wasm``, is installed automatically as a CLI dependency.

.. _system-toolkit-sec-03:

Quick Start
~~~~~~~~~~~~~~~~~~~~~~~~~~

Create and run a JavaScript GUI app:

.. code-block:: bash

   brookesia init my-app --template js-gui
   cd my-app
   pnpm install
   brookesia doctor
   brookesia build
   brookesia simulate --target system

``brookesia build`` creates a debug ``.bpk``. For a release build, initialize signing material first:

.. code-block:: bash

   brookesia sign init
   brookesia release
   brookesia verify

.. _system-toolkit-sec-04:

App Templates
~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``brookesia init`` command supports the following templates:

.. list-table:: Toolkit App Templates
   :header-rows: 1
   :widths: 20 35 45

   * - Template
     - Purpose
     - Notes
   * - ``js-gui``
     - JavaScript GUI app
     - Default template with GUI resources
   * - ``js-bundle``
     - Bundled JavaScript app
     - Uses an rspack- or webpack-style bundler
   * - ``lua-gui``
     - Lua GUI app
     - Self-contained app

For example, specify the template and parent directory:

.. code-block:: bash

   brookesia init test --template js-gui --dir my-app

.. _system-toolkit-sec-05:

Command Reference
~~~~~~~~~~~~~~~~~~~~~~~~~~

To view all commands and options:

.. code-block:: bash

   brookesia --help
   brookesia <command> --help

The common commands are:

.. list-table:: brookesia CLI Commands
   :header-rows: 1
   :widths: 25 75

   * - Command
     - Description
   * - ``brookesia init <name>``
     - Create an app directory from a template; use ``--template`` and ``--dir`` to select the template and parent directory.
   * - ``brookesia doctor``
     - Check the app environment, simulator, and bundler; use ``--profile`` to select a check profile.
   * - ``brookesia build``
     - Run a development build and create a debug ``.bpk``.
   * - ``brookesia release``
     - Run a release build and create a signed release ``.bpk``.
   * - ``brookesia sign init``
     - Generate signing keys in ``sign/``.
   * - ``brookesia pack``
     - Pack a directory into a ``.bpk`` with ``--source-dir`` and ``--output``; use ``--release`` for release signing.
   * - ``brookesia verify``
     - Verify a release ``.bpk`` signature; use ``--package`` to select the file.
   * - ``brookesia simulate``
     - Start a PC simulator; use ``--target`` to select the simulator type.
   * - ``brookesia toolchain install``
     - Record the local simulator binary and GUI project directory configuration.
   * - ``brookesia dev``
     - Start the Settings GUI simulator without building an app.

.. _system-toolkit-sec-06:

Simulator Modes
~~~~~~~~~~~~~~~~~~~~~~~~~~

``brookesia simulate --target system`` starts the WASM simulator in a browser and serves it through a local HTTP server:

.. code-block:: bash

   brookesia simulate --target system
   brookesia simulate --target system --smoke --duration-ms 2000
   brookesia simulate --target system --gui-debug
   brookesia simulate --target system --resolution 1024x600

The main supported targets include:

- ``system``: Run the app package in the WASM browser simulator.
- ``runtime``: Run runtime-side tests with an external runtime simulator.
- ``gui``: Start a configured GUI simulator project.
- ``settings``: Start the Settings GUI simulator.
- ``both``: Start both the GUI and system simulators.

``@brookesia/simulator-wasm`` is resolved from the globally installed CLI, not from the app's ``node_modules``. The app's ``pnpm install`` only installs bundler dependencies.
