"""Rebuild the ground's material in Unreal's Python editor.

UnrealEditor-Cmd <project> -EnablePlugins=PythonScriptPlugin -run=pythonscript
  -script=<this file> -SCCProvider=None -unattended -NullRHI -NoAudio
The saved material is plugin content and must ship with the C++ implementation.

-SCCProvider=None, because the host project's git source control runs from the host
repository, where this plugin's files are a submodule's and unknown to it, and it then
refuses to delete the old asset so the new one cannot be created in its place.

ALoomaTerrainActor colours each vertex of the ground on the CPU, as the web's
terrain shader colours each pixel (frontend/src/scene/terrain/shading.js): grass,
rock on steep ground, sand at the waterline. The colour is stored sRGB-encoded, so
the dark greens keep their steps in eight bits, and decoded here. Two-sided, because
the skirt round the edge is seen from inside where it rises to a higher seabed, and
the ground is seen from below by an eye under the water.
"""
import unreal

PATH = "/LoomaSceneSync/Materials/M_LoomaTerrain"
tools = unreal.AssetToolsHelpers.get_asset_tools()
# Built afresh rather than cleared in place, as create_skybox_material.py is: in a 5.8
# commandlet the loaded asset's expressions are rooted, and DeleteAllMaterialExpressions
# asserts on them. The C++ finds the material by path, so a new asset there is the same one.
if unreal.EditorAssetLibrary.does_asset_exist(PATH):
    assert unreal.EditorAssetLibrary.delete_asset(PATH), PATH
material = tools.create_asset("M_LoomaTerrain", "/LoomaSceneSync/Materials", unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
material.set_editor_property("two_sided", True)


def expression(kind):
    return unreal.MaterialEditingLibrary.create_material_expression(material, kind)


color = expression(unreal.MaterialExpressionVertexColor)
decode = expression(unreal.MaterialExpressionPower)
decode.set_editor_property("const_exponent", 2.2)
unreal.MaterialEditingLibrary.connect_material_expressions(color, "", decode, "Base")
unreal.MaterialEditingLibrary.connect_material_property(decode, "", unreal.MaterialProperty.MP_BASE_COLOR)

roughness = expression(unreal.MaterialExpressionScalarParameter)
roughness.set_editor_property("parameter_name", "Roughness")
roughness.set_editor_property("default_value", 0.92)
unreal.MaterialEditingLibrary.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

unreal.MaterialEditingLibrary.layout_material_expressions(material)
unreal.MaterialEditingLibrary.recompile_material(material)
unreal.EditorAssetLibrary.save_asset(PATH)
unreal.log("LOOMA_TERRAIN_MATERIAL_SAVED " + PATH)
