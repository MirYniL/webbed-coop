using System.Linq;
using UndertaleModLib;
using UndertaleModLib.Models;
var parent=Data.Extensions[0];
var file=new UndertaleExtensionFile{
 Filename=Data.Strings.MakeString("WebbedCoop.dll"),
 InitScript=Data.Strings.MakeString("webbed_coop_init"),
 CleanupScript=Data.Strings.MakeString(""),
 Kind=parent.Files[0].Kind
};
var init=new UndertaleExtensionFunction{
 Name=file.InitScript, ExtName=file.InitScript, Kind=11,
 ID=Data.Extensions.SelectMany(e=>e.Files).SelectMany(f=>f.Functions).Max(f=>f.ID)+1, RetType=parent.Files[0].Functions[0].RetType
};
file.Functions.Add(init); parent.Files.Add(file);

var player=Data.GameObjects[94];
var remote=new UndertaleGameObject();
foreach(var p in player.GetType().GetProperties()){
 if(p.CanWrite && p.Name!="Name" && p.Name!="Events" && p.Name!="PhysicsVertices")p.SetValue(remote,p.GetValue(player));
}
remote.Name=Data.Strings.MakeString("oCoopRemoteSpider");
foreach(var p in player.PhysicsVertices)remote.PhysicsVertices.Add(new UndertaleGameObject.UndertalePhysicsVertex{X=p.X,Y=p.Y});
var clone=typeof(object).GetMethod("MemberwiseClone",System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic);
for(int group=0;group<player.Events.Count;group++){
 foreach(var source in player.Events[group]){
  var ev=new UndertaleGameObject.Event{EventSubtype=source.EventSubtype};
  foreach(var action in source.Actions){var copy=(UndertaleGameObject.EventAction)clone.Invoke(action,null); var id=action.GetType().GetField("_codeId",System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic);id.SetValue(copy,clone.Invoke(id.GetValue(action),null));ev.Actions.Add(copy);}
  remote.Events[group].Add(ev);
 }
}
if(Data.GameObjects.Count!=467)throw new System.Exception("Unsupported Webbed object table.");
Data.GameObjects.Add(remote);


