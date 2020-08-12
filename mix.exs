defmodule ScenicDriverEGL.MixProject do
  use Mix.Project

  @github "https://github.com/verypossible-labs/scenic_driver_egl"
  @version "0.1.0"

  def project do
    [
      app: :scenic_driver_egl,
      version: @version,
      build_path: "_build",
      config_path: "config/config.exs",
      deps_path: "deps",
      lockfile: "mix.lock",
      elixir: "~> 1.8",
      description: description(),
      build_embedded: true,
      start_permanent: Mix.env() == :prod,
      compilers: [:elixir_make] ++ Mix.compilers(),
      make_env: %{"MIX_ENV" => to_string(Mix.env())},
      make_clean: ["clean"],
      deps: deps(),
      dialyzer: [plt_add_deps: :transitive],
      package: [
        name: :scenic_driver_egl,
        licenses: ["Apache 2"],
        links: %{github: @github},
        files: [
          "c_src/**/*.[ch]",
          "c_src/**/*.txt",
          "config",
          "priv/fonts/**/*.txt",
          "priv/fonts/**/*.ttf.*",
          "lib/**/*.ex",
          "Makefile",
          "mix.exs",
          "README.md",
          "LICENSE",
          "CHANGELOG.md"
        ]
      ],
      docs: [
        source_ref: "v#{@version}",
        source_url: @github
      ]
    ]
  end

  # Run "mix help compile.app" to learn about applications.
  def application do
    [
      extra_applications: [:logger]
    ]
  end

  # Run "mix help deps" to learn about dependencies.
  defp deps do
    [
      {:scenic, "~> 0.10"},
      {:elixir_make, "~> 0.6", runtime: false},
      {:ex_doc, ">= 0.0.0", only: [:dev]},
      {:dialyxir, "~> 0.5", only: :dev, runtime: false}
    ]
  end

  defp description() do
    """
    ScenicDriverEGL - Scenic driver for Linux DRM
    """
  end
end
