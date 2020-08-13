#
#  Created by Boyd Multerer on June 4, 2018.
#  Copyright © 2018 Kry10 Industries. All rights reserved.
#

defmodule ScenicDriverEGL.Font do
  alias ScenicDriverEGL
  alias Scenic.Cache
  require Logger

  # import IEx

  @font_folder "fonts"

  # @cmd_load_font_file 0x37
  @cmd_load_font_blob 0x38
  @cmd_free_font 0x39

  # --------------------------------------------------------
  def load_font(font_key, port)

  def load_font(hash, port) when is_bitstring(hash) do
    case Cache.Static.Font.fetch(hash) do
      {:ok, font} ->
        do_load_font(font, hash, port)

      _ ->
        font_folder =
          :code.priv_dir(:scenic_driver_egl)
          |> Path.join(@font_folder)

        with {:ok, ^hash} <- Cache.Static.Font.load(font_folder, hash),
             {:ok, font} <- Cache.Static.Font.fetch(hash) do
          do_load_font(font, hash, port)
        end
    end
  end

  defp do_load_font(font_blob, font_hash, port) do
    <<
      @cmd_load_font_blob::unsigned-integer-size(32)-native,
      byte_size(font_hash) + 1::unsigned-integer-size(32)-native,
      byte_size(font_blob)::unsigned-integer-size(32)-native,
      font_hash::binary,
      # null terminate so it can be used directly
      0::size(8),
      font_blob::binary
    >>
    |> ScenicDriverEGL.Port.send(port)
  end

  # --------------------------------------------------------
  def free_font(font_name_or_key, port) do
    name = to_string(font_name_or_key)
    # send the message to the C driver to load the font
    <<
      @cmd_free_font::unsigned-integer-size(32)-native,
      byte_size(name) + 1::unsigned-integer-size(16)-native,
      name::binary,
      # null terminate so it can be used directly
      0::size(8)
    >>
    |> ScenicDriverEGL.Port.send(port)
  end
end
