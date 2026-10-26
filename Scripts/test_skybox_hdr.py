"""Run in the editor with -RenderOffscreen -ExecutePythonScript=<this file>.
Serve the asset-demo frontend on 5181. Exercises actual HTTP import and sky light.
"""
import time
import traceback
import unreal


def run():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    owned = []
    hidden = []
    for actor in actors.get_all_level_actors():
        for light in actor.get_components_by_class(unreal.LightComponent):
            if light.is_visible():
                hidden.append(light)
                light.set_visibility(False)

    host = actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector())
    owned.append(host)
    host_light = host.get_component_by_class(unreal.SkyLightComponent)
    dome = actors.spawn_actor_from_class(unreal.LoomaSkyboxActor, unreal.Vector())
    owned.append(dome)
    light = dome.get_component_by_class(unreal.SkyLightComponent)
    spec = unreal.LoomaSkyboxSpec()
    spec.lighting = True
    spec.lighting_intensity = 1
    spec.exposure = -4
    url = "http://127.0.0.1:5181/qa/skybox/lighting.hdr"
    unreal.MaterialEditingLibrary.get_statistics(unreal.load_asset("/LoomaSceneSync/Materials/M_LoomaPanorama"))
    dome.apply(url, spec)

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset("M_HDRProbe", "/Game/Developers/LoomaQA", unreal.Material, unreal.MaterialFactoryNew())
    for prop, value in [(unreal.MaterialProperty.MP_METALLIC, 1.0), (unreal.MaterialProperty.MP_ROUGHNESS, 0.1)]:
        expression = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant)
        expression.set_editor_property("r", value)
        assert unreal.MaterialEditingLibrary.connect_material_property(expression, "", prop), prop
    expression = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
    expression.set_editor_property("constant", unreal.LinearColor(1,1,1,1))
    assert unreal.MaterialEditingLibrary.connect_material_property(expression, "", unreal.MaterialProperty.MP_BASE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(material)
    unreal.MaterialEditingLibrary.get_statistics(material)
    sphere = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector())
    owned.append(sphere)
    sphere.static_mesh_component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Sphere"))
    sphere.static_mesh_component.set_material(0, material)
    capture_actor = actors.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0,300,0))
    owned.append(capture_actor)
    capture_actor.set_actor_rotation(unreal.Rotator(pitch=0,yaw=-90,roll=0), False)
    target = unreal.RenderingLibrary.create_render_target2d(world,256,256,unreal.TextureRenderTargetFormat.RTF_RGBA16F)
    capture = capture_actor.get_component_by_class(unreal.SceneCaptureComponent2D)
    capture.set_editor_property("texture_target", target)
    capture.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_SCENE_COLOR_HDR_NO_ALPHA)
    capture.set_editor_property("primitive_render_mode", unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST)
    capture.show_only_actor_components(sphere)
    capture.show_only_actor_components(dome)
    capture.set_editor_property("capture_every_frame", False)
    capture.set_editor_property("capture_on_movement", False)
    post = unreal.PostProcessSettings()
    post.set_editor_property("override_reflection_method", True)
    post.set_editor_property("reflection_method", unreal.ReflectionMethod.SCREEN_SPACE)
    capture.set_editor_property("post_process_settings", post)
    capture.set_editor_property("post_process_blend_weight", 1.0)
    capture.set_editor_property("always_persist_rendering_state", True)
    flags = []
    for name in ("Lighting", "SkyLighting", "ReflectionEnvironment", "Specular"):
        flag = unreal.EngineShowFlagsSetting()
        flag.show_flag_name = name
        flag.enabled = True
        flags.append(flag)
    capture.set_editor_property("show_flag_settings", flags)


    def pixel():
        capture.capture_scene()
        return unreal.RenderingLibrary.read_render_target_raw_pixel(world,target,128,128,False)

    busy = False
    phase = 0
    frames = 0
    deadline = time.monotonic()+120
    handle = None

    def finish():
        unreal.unregister_slate_post_tick_callback(handle)
        dome.apply("", unreal.LoomaSkyboxSpec())
        for actor in reversed(owned):
            actors.destroy_actor(actor)
        for component in hidden:
            component.set_visibility(True)
        unreal.SystemLibrary.quit_editor()

    def tick(_delta):
        nonlocal phase, frames, busy
        if busy:
            return
        busy = True
        try:
            assert time.monotonic() < deadline, "HDR lighting timed out"
            if phase == 0:
                if not light.get_editor_property("cubemap"):
                    return
                assert not host_light.is_visible(), "Existing host sky light was not suspended"
                phase = 1
                return
            frames += 1
            if frames < 80:
                return
            if phase == 1:
                value = pixel()
                unreal.log("UNREAL_HDR_REFLECTION " + str(value))
                assert value.r > .005 and value.r > value.g*1.5, "HDR did not illuminate the surface"
                spec.lighting_intensity = 0
                dome.apply(url,spec)
                phase = 2
                frames = 0
            elif phase == 2:
                value = pixel()
                unreal.log("UNREAL_HDR_DARK " + str(value))
                assert value.r < .01, "Zero lighting intensity still illuminates"
                spec.lighting = False
                dome.apply(url,spec)
                assert not light.is_visible() and host_light.is_visible(), "Host lighting was not restored"
                assert dome.get_component_by_class(unreal.StaticMeshComponent).is_visible(), "Lighting toggle removed background"
                unreal.log("UNREAL_HDR_PASSED")
                finish()
        except Exception:
            unreal.log_error("UNREAL_HDR_FAILED " + traceback.format_exc())
            finish()
        finally:
            busy = False

    handle = unreal.register_slate_post_tick_callback(tick)

# ExecCmds runs before the initial editor map finishes opening. Wait for it, then
# create actors in the final editor world rather than its startup placeholder.
startup = time.monotonic()
startup_handle = None
def start_when_ready(_delta):
    if time.monotonic() - startup < 8:
        return
    unreal.unregister_slate_post_tick_callback(startup_handle)
    run()
startup_handle = unreal.register_slate_post_tick_callback(start_when_ready)
