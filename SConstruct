# Begin code that does stuff
import Config
import DefaultConfig
import Universal

vars = Variables()
Config.GetVars(vars)
Universal.GetVars(vars)
env = Environment(variables=vars)

Export('env')

libs = env.SConscript('tnp/SConscript')
env.SConscript('test/test2/SConscript', 'libs')
