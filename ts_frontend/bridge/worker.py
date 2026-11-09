"""Single-owner replay worker. Python 3.6 compatible; no Qt or qrenderdoc imports."""
import sys
import os
import json
import time
import queue
import threading
import traceback
import zlib
import struct
import _frontend_native as native
import renderdoc as rd

protocol_out = sys.stdout
sys.stdout = sys.stderr
commands = queue.Queue()
write_lock = threading.Lock()
latest_select = [None]


def send(message):
    with write_lock:
        protocol_out.write(json.dumps(message, ensure_ascii=False) + '\n')
        protocol_out.flush()


def reader():
    try:
        for line in sys.stdin:
            try:
                msg = json.loads(line)
                if msg.get('method') == 'selectEvent':
                    latest_select[0] = msg.get('id')
                commands.put(msg)
            except Exception as exc:
                send({'event': 'protocolError', 'message': str(exc)})
    finally:
        commands.put({'id': 0, 'method': 'shutdown'})


class Worker:
    def __init__(self):
        self.controller = None
        self.output = None
        self.parent = None
        self.external_surface = None
        self.viewport = dict(x=0, y=0, width=1, height=1, scale=1, visible=False)
        self.textures = {}
        self.names = {}
        self.actions = {}
        self.selected = 0
        self.generation = 0
        self.display = None
        self.fit = True
        self.path = None

    def close(self):
        if self.output:
            self.output.Shutdown()
            self.output = None
        native.destroy()
        if self.controller:
            self.controller.Shutdown()
            self.controller = None
        self.textures, self.actions, self.names = {}, {}, {}
        self.path, self.display, self.selected = None, None, 0
        self.generation += 1
        return {'generation': self.generation}

    def ensure_output(self):
        if self.controller and self.parent and not self.output:
            if self.external_surface:
                native.attach(self.external_surface)
                hwnd = self.external_surface
            else:
                raise RuntimeError('No native replay surface is attached')
            self.apply_bounds()
            self.output = self.controller.CreateOutput(rd.CreateWin32WindowingData(int(hwnd)), rd.ReplayOutputType.Texture)
            if not self.output:
                native.destroy()
                raise RuntimeError('RenderDoc could not create a native replay output')

    def apply_bounds(self):
        v = self.viewport
        native.bounds(0 if self.external_surface else v['x'], 0 if self.external_surface else v['y'], v['width'], v['height'], v['scale'], v['visible'])

    def redraw(self):
        if not self.output or not self.display or not self.viewport['visible']:
            return
        info = native.inspect()
        tex = self.textures.get(str(self.display.resourceId))
        if tex and self.fit:
            self.display.scale = min(info['width'] / float(tex.width), info['height'] / float(tex.height))
            self.display.xOffset = (info['width'] - tex.width * self.display.scale) / 2
            self.display.yOffset = (info['height'] - tex.height * self.display.scale) / 2
        else:
            self.display.scale = 1.0
            self.display.xOffset = self.display.yOffset = 0
        self.output.SetTextureDisplay(self.display)
        self.output.Display()

    def name(self, resource):
        return self.names.get(str(resource), str(resource))

    def texture_info(self, tex):
        return dict(id=str(tex.resourceId), name=self.name(tex.resourceId), width=tex.width,
                    height=tex.height, format=tex.format.Name(), mips=tex.mips, arraysize=tex.arraysize)

    def flatten(self, actions, parent=None, depth=0):
        rows = []
        for action in actions:
            self.actions[action.eventId] = action
            rows.append(dict(eventId=action.eventId, parent=parent, depth=depth,
                             name=action.GetName(self.structured), flags=str(action.flags),
                             children=len(action.children), draw=bool(action.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch))))
            rows.extend(self.flatten(action.children, action.eventId, depth+1))
        return rows

    def open(self, path):
        self.close()
        cap = rd.OpenCaptureFile()
        started = time.monotonic()
        try:
            result = cap.OpenFile(path, '', None)
            if result != rd.ResultCode.Succeeded:
                raise RuntimeError('Cannot open capture: ' + str(result))
            if not cap.LocalReplaySupport():
                raise RuntimeError('This capture cannot be replayed locally on this machine')
            # Standalone Python 3.6's progress callback can deadlock when replay setup
            # invokes it on another thread while this binding owns the GIL. Use the
            # documented no-callback path; the UI shows an indeterminate loading state.
            send({'event': 'progress', 'value': 0})
            result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
            send({'event': 'progress', 'value': 1})
            if result != rd.ResultCode.Succeeded:
                raise RuntimeError('Cannot initialise replay: ' + str(result))
            self.controller = controller
        finally:
            cap.Shutdown()
        try:
            self.path = path
            self.names = {str(r.resourceId): r.name for r in self.controller.GetResources()}
            self.textures = {str(t.resourceId): t for t in self.controller.GetTextures()}
            self.structured = self.controller.GetStructuredFile()
            rows = self.flatten(self.controller.GetRootActions())
            self.ensure_output()
            draws = [r['eventId'] for r in rows if r['draw']]
            initial = self.select(draws[-1] if draws else (rows[-1]['eventId'] if rows else 0))
            return dict(path=path, generation=self.generation, api=str(self.controller.GetAPIProperties().pipelineType),
                        events=rows, textures=[self.texture_info(t) for t in self.textures.values()],
                        initial=initial, elapsedMs=round((time.monotonic()-started)*1000))
        except Exception:
            self.close()
            raise

    def select(self, event_id):
        if not self.controller:
            raise RuntimeError('No capture is open')
        start = time.monotonic()
        self.controller.SetFrameEvent(event_id, True)
        self.selected = event_id
        pipe = self.controller.GetPipelineState()
        action = self.actions.get(event_id)
        outputs = []
        for target in pipe.GetOutputTargets():
            tex = self.textures.get(str(target.resource))
            if tex:
                outputs.append(self.texture_info(tex))
        depth_target = pipe.GetDepthTarget()
        depth = self.textures.get(str(depth_target.resource))
        stages = []
        for label, stage in [('VS',rd.ShaderStage.Vertex),('HS',rd.ShaderStage.Hull),('DS',rd.ShaderStage.Domain),
                             ('GS',rd.ShaderStage.Geometry),('PS',rd.ShaderStage.Pixel),('CS',rd.ShaderStage.Compute)]:
            shader = pipe.GetShader(stage)
            if shader != rd.ResourceId.Null():
                stages.append(dict(stage=label, id=str(shader), name=self.name(shader)))
        if outputs:
            self.set_texture(outputs[0]['id'])
        else:
            self.display = None
            if self.output:
                empty = rd.TextureDisplay()
                self.output.SetTextureDisplay(empty)
                self.output.Display()
        return dict(generation=self.generation, eventId=event_id, name=action.GetName(self.structured) if action else '',
                    stages=stages, outputs=outputs, depth=self.texture_info(depth) if depth else None,
                    textureId=str(self.display.resourceId) if self.display else None,
                    numIndices=action.numIndices if action else 0, numInstances=action.numInstances if action else 0,
                    elapsedMs=round((time.monotonic()-start)*1000))

    def set_texture(self, resource_id, fit=None, channels=None):
        tex = self.textures.get(resource_id)
        if not tex:
            raise RuntimeError('Texture no longer exists in this capture')
        if fit is not None:
            self.fit = fit
        display = rd.TextureDisplay()
        display.resourceId = tex.resourceId
        display.subresource.sample = 0xffffffff  # ResolveSamples sentinel in the C++ replay API.
        if channels:
            display.red,display.green,display.blue,display.alpha = channels
        self.display = display
        self.ensure_output()
        self.redraw()
        return {'textureId': resource_id, 'generation': self.generation, 'eventId': self.selected}

    def shader(self, label):
        stage = dict(VS=rd.ShaderStage.Vertex, HS=rd.ShaderStage.Hull, DS=rd.ShaderStage.Domain,
                     GS=rd.ShaderStage.Geometry, PS=rd.ShaderStage.Pixel, CS=rd.ShaderStage.Compute)[label]
        pipe = self.controller.GetPipelineState()
        reflection = pipe.GetShaderReflection(stage)
        if not reflection:
            return {'text': '', 'kind': 'empty', 'stage': label, 'eventId': self.selected, 'generation': self.generation}
        sources = list(reflection.debugInfo.files)
        if sources:
            text = '\n\n'.join('// ' + f.filename + '\n' + f.contents for f in sources)
            kind = 'source'
        else:
            pipeline = pipe.GetComputePipelineObject() if label == 'CS' else pipe.GetGraphicsPipelineObject()
            targets = list(self.controller.GetDisassemblyTargets(True))
            text = self.controller.DisassembleShader(pipeline, reflection, targets[0] if targets else '')
            kind = 'disassembly'
        return dict(text=text, kind=kind, stage=label, eventId=self.selected, generation=self.generation)

    def dispatch(self, method, p):
        if method == 'hello':
            return dict(protocol=1, python=sys.version.split()[0], version=rd.GetVersionString(), pid=os.getpid())
        if method == 'openCapture': return self.open(p['path'])
        if method == 'closeCapture': return self.close()
        if method == 'selectEvent': return self.select(int(p['eventId']))
        if method == 'getShader': return self.shader(p['stage'])
        if method == 'setTexture': return self.set_texture(p['textureId'],p.get('fit'),p.get('channels'))
        if method == 'viewport':
            if 'surface' in p:
                self.external_surface = p['surface']
            if p.get('parent'):
                self.parent = p['parent']
            self.viewport.update({k:p[k] for k in self.viewport if k in p})
            self.ensure_output()
            self.apply_bounds()
            self.redraw()
            return native.inspect()
        if method == 'inspect':
            info = native.inspect()
            # Diagnostics only: readback is never used to display the frontend preview.
            if p.get('readback') and self.output:
                pixels = bytes(self.output.ReadbackOutputTexture())
                info.update(crc32=zlib.crc32(pixels), bytes=len(pixels), uniqueBytes=len(set(pixels)))
                if p.get('png'):
                    width,height = self.output.GetDimensions()
                    def chunk(kind,data):
                        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
                    raw=b''.join(b'\0'+pixels[y*width*3:(y+1)*width*3] for y in range(height))
                    with open(p['png'],'wb') as image:
                        image.write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
            return dict(info, generation=self.generation, eventId=self.selected, path=self.path)
        raise RuntimeError('Unknown frontend method: '+method)


log_directory = os.environ.get('RENDERDIC_FRONTEND_LOG_DIR')
if log_directory:
    os.makedirs(log_directory, exist_ok=True)
    rd.SetDebugLogFile(os.path.join(log_directory, 'replay-core-{}.log'.format(os.getpid())))
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
worker = Worker()
threading.Thread(target=reader, daemon=True).start()
send({'event':'ready','protocol':1})
try:
    last_present = 0
    while True:
        now = time.monotonic()
        if native.pump() or now-last_present > 0.1:
            worker.redraw()
            last_present = now
        try:
            message = commands.get(timeout=0.015)
        except queue.Empty:
            continue
        ident, method = message.get('id'), message.get('method')
        if method == 'shutdown':
            worker.close()
            send({'id':ident,'result':True})
            break
        try:
            if method == 'selectEvent' and ident != latest_select[0]:
                send({'id':ident,'error':{'code':'SUPERSEDED','message':'A newer event was selected'}})
                continue
            params = message.get('params', {})
            expected = params.get('generation')
            if expected is not None and expected != worker.generation:
                raise RuntimeError('Stale capture generation')
            if method in ('getShader','setTexture') and params.get('eventId',worker.selected) != worker.selected:
                send({'id':ident,'error':{'code':'SUPERSEDED','message':'A newer event was selected'}})
                continue
            send({'id':ident,'result':worker.dispatch(method,params)})
        except Exception as exc:
            traceback.print_exc(file=sys.stderr)
            send({'id':ident,'error':{'code':'REPLAY_ERROR','message':str(exc)}})
finally:
    worker.close()
    rd.ShutdownReplay()
