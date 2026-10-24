#pragma once
#include <cmath>
#include <stdexcept>

// Deliberately bounded ARBvp/fp translator. Unsupported grammar is rejected,
// never accepted as an empty shader. No assembly extension is advertised.
struct VexaAssemblyTranslation {
  std::string GLSL, Error;
  int Position = -1, Instructions = 0, Temporaries = 0, TextureInstructions = 0;
};

class VexaAssemblyTranslator {
  bool Vertex;
  std::unordered_map<std::string, std::string> Aliases;
  std::unordered_set<std::string> Registers, Samplers;
  std::string Declarations, Body;
  int Offset = 0;
  VexaAssemblyTranslation Result;

  static std::string Trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
  }
  static std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> values; int depth = 0; size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '{' || s[i] == '[' || s[i] == '(') ++depth;
      if (s[i] == '}' || s[i] == ']' || s[i] == ')') --depth;
      if (s[i] == ',' && !depth) { values.push_back(Trim(s.substr(start, i-start))); start = i+1; }
      if (depth < 0) throw std::runtime_error("unbalanced operand");
    }
    if (depth) throw std::runtime_error("unbalanced operand");
    values.push_back(Trim(s.substr(start))); return values;
  }
  static std::pair<std::string,std::string> Mask(std::string s) {
    const auto dot = s.rfind('.');
    if (dot != std::string::npos) {
      auto mask = s.substr(dot+1);
      if (!mask.empty() && mask.size() <= 4 && mask.find_first_not_of("xyzwrgba") == std::string::npos) {
        for (char& c : mask) { auto n=std::string("rgba").find(c); if(n!=std::string::npos)c="xyzw"[n]; }
        return {s.substr(0,dot),mask};
      }
    }
    return {s,{}};
  }
  std::string Base(std::string s, bool output = false) {
    s = Trim(s);
    if (auto i=Aliases.find(s);i!=Aliases.end()) return i->second;
    if (Registers.contains(s)) return s;
    std::smatch m;
    if (std::regex_match(s,m,std::regex(R"((program\.(local|env))\[(.+)\])"))) {
      std::string index=m[3];
      if (!std::regex_match(index,std::regex(R"([0-9]+|[A-Za-z_][A-Za-z_0-9]*\.x\s*(?:[+-]\s*[0-9]+)?)"))) throw std::runtime_error("unsupported parameter index");
      if (std::regex_match(index,std::regex("[0-9]+")) && std::stoul(index)>=128) throw std::runtime_error("parameter index exceeds 127");
      return std::string(Vertex?"uV":"uF")+(m[2]=="local"?"Local[":"Env[")+index+"]";
    }
    if(std::regex_match(s,m,std::regex(R"(([A-Za-z_][A-Za-z_0-9]*)\[(.+)\])")) && Registers.contains(m[1])) return s;
    if (Vertex) {
      if (s=="vertex.position") return "a0";
      if (s=="vertex.normal") return "a2";
      if (s=="vertex.color" || s=="vertex.color.primary") return "a3";
      if (s=="vertex.color.secondary") return "a4";
      if (s=="vertex.fogcoord") return "a5";
      if (std::regex_match(s,m,std::regex(R"(vertex\.(attrib|texcoord)\[([0-9]+)\])"))) {
        const auto n=std::stoul(m[2])+(m[1]=="texcoord"?8:0);
        if(n>=16)throw std::runtime_error("vertex input index exceeds 15");
        return "a"+std::to_string(n);
      }
      if(output) {
        if(s=="result.position")return "gl_Position";
        if(s=="result.pointsize")return "gl_PointSize";
        if(s=="result.color" || s=="result.color.primary" || s=="result.color.front.primary")return "vColor";
        if(s=="result.color.secondary" || s=="result.color.front.secondary")return "vSecondary";
        if(s=="result.fogcoord")return "vFog";
        if(std::regex_match(s,m,std::regex(R"(result\.texcoord\[([0-9]+)\])")) && std::stoul(m[1])<8)return "vTex["+m[1].str()+"]";
      }
    } else {
      if(s=="fragment.position")return "gl_FragCoord";
      if(s=="fragment.color" || s=="fragment.color.primary")return "vColor";
      if(s=="fragment.color.secondary")return "vSecondary";
      if(s=="fragment.fogcoord")return "vec4(vFog)";
      if(std::regex_match(s,m,std::regex(R"(fragment\.texcoord\[([0-9]+)\])")) && std::stoul(m[1])<8)return "vTex["+m[1].str()+"]";
      if(output) {
        if(s=="result.color")return "outColor[0]";
        if(s=="result.depth")return "gl_FragDepth";
        if(std::regex_match(s,m,std::regex(R"(result\.color\[([0-9]+)\])")) && std::stoul(m[1])<4)return "outColor["+m[1].str()+"]";
      }
    }
    if (!s.empty() && s.front()=='{' && s.back()=='}') {
      auto v=Split(s.substr(1,s.size()-2));
      if(v.size()!=1 && v.size()!=4)throw std::runtime_error("constant must have one or four components");
      return "vec4("+s.substr(1,s.size()-2)+")";
    }
    if(std::regex_match(s,std::regex(R"((?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)"))) return "vec4("+s+")";
    throw std::runtime_error("unsupported register: "+s);
  }
  std::string Source(std::string s) {
    s=Trim(s); bool negative=false;
    if(!s.empty() && (s[0]=='-'||s[0]=='+')) {negative=s[0]=='-';s=Trim(s.substr(1));}
    auto [base,mask]=Mask(s);
    std::string value=Base(base);
    if(!mask.empty()) {
      if(mask.size()!=1 && mask.size()!=4)throw std::runtime_error("unsupported source swizzle");
      value="vec4(("+value+")."+mask+")";
    }
    return negative?"(-("+value+"))":"("+value+")";
  }
  void Assign(std::string destination, std::string expression, bool saturate) {
    auto [base,mask]=Mask(Trim(destination));
    const auto target=Base(base,true);
    if(saturate)expression="clamp("+expression+",0.0,1.0)";
    if(target=="gl_PointSize"||target=="gl_FragDepth"||target=="vFog") {
      const std::string component=target=="gl_FragDepth"?"z":"x";
      Body+=target+"=("+expression+")."+(mask.empty()?component:mask)+";\n";
    } else Body+=target+(mask.empty()?"":"."+mask)+"=("+expression+")"+(mask.empty()?"":"."+mask)+";\n";
  }
  void Statement(std::string s) {
    s=Trim(s); if(s.empty())return;
    const auto space=s.find_first_of(" \t\r\n");
    std::string op=s.substr(0,space), tail=space==std::string::npos?"":Trim(s.substr(space));
    if(op=="OPTION") {
      if(tail!="ARB_precision_hint_fastest"&&tail!="ARB_precision_hint_nicest")throw std::runtime_error("unsupported OPTION: "+tail);
      return;
    }
    if(op=="TEMP"||op=="ADDRESS") {
      for(auto& name:Split(tail)) {
        if(!std::regex_match(name,std::regex("[A-Za-z_][A-Za-z_0-9]*"))||Registers.contains(name)||Aliases.contains(name))throw std::runtime_error("invalid or duplicate register");
        Registers.insert(name); Declarations+=(op=="TEMP"?"vec4 ":"ivec4 ")+name+";\n";
        if(op=="TEMP" && ++Result.Temporaries>32)throw std::runtime_error("more than 32 temporaries");
      } return;
    }
    if(op=="ATTRIB"||op=="OUTPUT"||op=="ALIAS"||op=="PARAM") {
      const auto eq=tail.find('=');if(eq==std::string::npos)throw std::runtime_error("missing declaration initializer");
      auto name=Trim(tail.substr(0,eq)), value=Trim(tail.substr(eq+1));
      if(!std::regex_match(name,std::regex("[A-Za-z_][A-Za-z_0-9]*")))throw std::runtime_error("array declarations are not supported");
      if(Aliases.contains(name)||Registers.contains(name))throw std::runtime_error("duplicate register");
      Aliases[name]=op=="PARAM"?Source(value):Base(value,op=="OUTPUT"); return;
    }
    bool sat=op.ends_with("_SAT");if(sat)op.resize(op.size()-4);
    const auto args=Split(tail);
    if(++Result.Instructions>1024)throw std::runtime_error("more than 1024 instructions");
    if(op=="KIL") {if(Vertex||args.size()!=1)throw std::runtime_error("invalid KIL");Body+="if(any(lessThan("+Source(args[0])+",vec4(0.0))))discard;\n";return;}
    if(args.size()<2)throw std::runtime_error("missing instruction operands");
    auto arg=[&](size_t n){if(n>=args.size())throw std::runtime_error("missing source operand");return Source(args[n]);};
    const auto a=arg(1);std::string expression;size_t expected=0;
    if(op=="MOV") {expression=a;expected=2;}
    else if(op=="ABS"||op=="FLR"||op=="FRC"||op=="SGN") {expression=(op=="ABS"?"abs":op=="FLR"?"floor":op=="FRC"?"fract":"sign")+std::string("(")+a+")";expected=2;}
    else if(op=="RCP"||op=="RSQ"||op=="EX2"||op=="LG2"||op=="SIN"||op=="COS") {
      auto x="("+a+").x";
      expression="vec4("+(op=="RCP"?"1.0/"+x:op=="RSQ"?"inversesqrt(abs("+x+"))":op=="LG2"?"log2(abs("+x+"))":(op=="EX2"?"exp2":op=="SIN"?"sin":"cos")+std::string("(")+x+")")+")";expected=2;
    }
    else if(op=="ADD"||op=="SUB"||op=="MUL") {expression=a+(op=="ADD"?"+":op=="SUB"?"-":"*")+arg(2);expected=3;}
    else if(op=="MAD") {expression=a+"*"+arg(2)+"+"+arg(3);expected=4;}
    else if(op=="MIN"||op=="MAX") {expression=(op=="MIN"?"min(":"max(")+a+","+arg(2)+")";expected=3;}
    else if(op=="DP3"||op=="DP4"||op=="DPH") {auto b=arg(2);expression="vec4(dot("+(op=="DPH"?"vec4(("+a+").xyz,1.0)":op=="DP3"?a+".xyz":a)+","+b+(op=="DP3"?".xyz":"")+"))";expected=3;}
    else if(op=="SLT"||op=="SGE") {expression="vec4("+std::string(op=="SLT"?"lessThan(":"greaterThanEqual(")+a+","+arg(2)+"))";expected=3;}
    else if(op=="CMP") {expression="mix("+arg(3)+","+arg(2)+",lessThan("+a+",vec4(0.0)))";expected=4;}
    else if(op=="LRP") {expression=a+"*"+arg(2)+"+(vec4(1.0)-"+a+")*"+arg(3);expected=4;}
    else if(op=="POW") {expression="vec4(pow(abs(("+a+").x),("+arg(2)+").x))";expected=3;}
    else if(op=="XPD") {expression="vec4(cross("+a+".xyz,"+arg(2)+".xyz),1.0)";expected=3;}
    else if(op=="DST") {expression="vec4(1.0,("+a+").y*("+arg(2)+").y,("+a+").z,("+arg(2)+").w)";expected=3;}
    else if(op=="DDX"||op=="DDY") {if(Vertex)throw std::runtime_error("derivatives require fragment program");expression=(op=="DDX"?"dFdx(":"dFdy(")+a+")";expected=2;}
    else if(op=="ARL") {if(!Vertex||sat||args.size()!=2)throw std::runtime_error("invalid ARL");auto [base,mask]=Mask(args[0]);if(!Registers.contains(base)||mask!="x")throw std::runtime_error("invalid address destination");Body+=base+".x=int(floor(("+a+").x));\n";return;}
    else if(op=="TEX"||op=="TXP"||op=="TXB") {
      if(Vertex||args.size()!=4)throw std::runtime_error("unsupported texture instruction");
      std::smatch m;if(!std::regex_match(args[2],m,std::regex(R"(texture\[([0-9]+)\])"))||std::stoul(m[1])>=8)throw std::runtime_error("unsupported texture unit");
      const auto dimension=args[3];if(dimension!="2D"&&dimension!="3D"&&dimension!="CUBE"&&dimension!="RECT")throw std::runtime_error("unsupported texture target");
      auto name="vexa_asm_tex"+m[1].str()+"_"+(dimension=="RECT"?"2D":dimension);
      if(Samplers.insert(name).second)Declarations+="uniform sampler"+(dimension=="RECT"?std::string("2D"):dimension=="CUBE"?std::string("Cube"):dimension)+" "+name+";\n";
      std::string coord="("+a+")."+(dimension=="2D"||dimension=="RECT"?"xy":"xyz");
      if(op=="TXP")coord="("+coord+"/("+a+").w)";
      if(dimension=="RECT")coord="("+coord+"/vec2(textureSize("+name+",0)))";
      expression="texture("+name+","+coord+(op=="TXB"?",("+a+").w":"")+")";expected=4;++Result.TextureInstructions;
    } else throw std::runtime_error("unsupported opcode: "+op);
    if(args.size()!=expected)throw std::runtime_error("wrong operand count for "+op);
    Assign(args[0],expression,sat);
  }
public:
  explicit VexaAssemblyTranslator(bool vertex):Vertex(vertex){}
  VexaAssemblyTranslation Translate(std::string text) {
    try {
      if(!text.starts_with(Vertex?"!!ARBvp1.0":"!!ARBfp1.0"))throw std::runtime_error("invalid ARB program header");
      for(size_t i=10;i<text.size();++i)if(text[i]=='#')while(i<text.size()&&text[i]!='\n')text[i++]=' ';
      size_t pos=10;bool ended=false;
      while(pos<text.size()) {
        auto first=text.find_first_not_of(" \t\r\n",pos);if(first==std::string::npos)break;Offset=first;
        if(Trim(text.substr(first))=="END") {ended=true;break;}
        auto end=text.find(';',first);if(end==std::string::npos)throw std::runtime_error("missing semicolon or END");
        Statement(text.substr(first,end-first));pos=end+1;
      }
      if(!ended)throw std::runtime_error("missing END");
      std::string interface=Vertex?"out highp vec4 vColor;out highp vec4 vSecondary;out highp vec4 vTex[8];out highp float vFog;\n":"in highp vec4 vColor;in highp vec4 vSecondary;in highp vec4 vTex[8];in highp float vFog;layout(location=0) out highp vec4 outColor[4];\n";
      if(Vertex)for(int i=0;i<16;++i)interface+="layout(location="+std::to_string(i)+") in vec4 a"+std::to_string(i)+";\n";
      const std::string prefix=Vertex?"uV":"uF";
      Result.GLSL="#version 300 es\nprecision highp float;precision highp int;\nuniform vec4 "+prefix+"Local[128];uniform vec4 "+prefix+"Env[128];\n"+interface;
      // TEMP declarations belong inside main, whereas samplers must be global.
      std::string locals;
      for(size_t pos=0;pos<Declarations.size();) {auto end=Declarations.find('\n',pos);auto line=Declarations.substr(pos,end-pos+1);if(line.starts_with("uniform"))Result.GLSL+=line;else locals+=line;pos=end+1;}
      Result.GLSL+="void main(){\n"+locals+Body+"}\n";
    } catch(const std::exception& e) {Result.GLSL.clear();Result.Error=e.what();Result.Position=Offset;}
    return Result;
  }
};
