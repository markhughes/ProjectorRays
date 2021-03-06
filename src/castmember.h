#ifndef CASTMEMBER_H
#define CASTMEMBER_H

namespace ProjectorRays {

class ReadStream;
class Movie;

enum MemberType {
    kNullMember         = 0,
    kBitmapMember       = 1,
    kFilmLoopMember     = 2,
    kTextMember         = 3,
    kPaletteMember      = 4,
    kPictureMember      = 5,
    kSoundMember        = 6,
    kButtonMember       = 7,
    kShapeMember        = 8,
    kMovieMember        = 9,
    kDigitalVideoMember = 10,
    kScriptMember       = 11,
    kRTEMember          = 12
};

struct CastMember {
    Movie *movie;
    MemberType type;

    CastMember(Movie *m, MemberType t) : movie(m), type(t) {}
    virtual ~CastMember() = default;
    virtual void read(ReadStream &stream);
};

enum ScriptType {
    kUnknownScript,
    kScoreScript,
    kMovieScript,
    kParentScript
};

struct ScriptMember : CastMember {
    ScriptType scriptType;

    ScriptMember(Movie *m) : CastMember(m, kScriptMember) {}
    virtual ~ScriptMember() = default;
    virtual void read(ReadStream &stream);
};

}

#endif
