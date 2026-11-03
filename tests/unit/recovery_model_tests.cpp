#include "recovery/recovery_model.h"
#include "../../tools/recovery_demo/demo_reconnect.h"
#include "../../tools/recovery_demo/demo_composition.h"
#include "core/localization.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace localization { std::wstring Text(std::wstring_view id) { return std::wstring(id); } }
namespace {
int checks=0;
void Check(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
Annotation Text(const wchar_t* content) {
    Annotation annotation; annotation.id=L"one"; annotation.text=content;
    annotation.type=Annotation::Type::TextBox; return annotation;
}
AnnotCommand Update(Annotation before,Annotation after) {
    AnnotCommand command; command.kind=AnnotCommandKind::Update;
    command.beforeIndex=command.afterIndex=0;
    command.before=std::move(before); command.after=std::move(after); return command;
}
}
int main() {
    try {
        const auto before=Text(L"current"), expected=Text(L"expected"), after=Text(L"chosen");
        const auto next=Text(L"dependent");
        recovery::AnnotationPlan plan({before},AnnotNumericPrecision::RoundTrip17,{Update(expected,after),Update(after,next)});
        Check(!plan.Advance() && plan.cursor()==0 && plan.candidate()[0].text==L"current","conflict publishes no automatic edit");
        Check(plan.Resolve(recovery::Choice::Apply) && plan.cursor()==1,"explicit adoption resolves only one operation");
        Check(plan.Advance() && plan.complete() && plan.candidate()[0].text==L"dependent","successor is revalidated against chosen content");
        Check(plan.Undo() && !plan.complete() && plan.candidate()[0].text==L"chosen","undo restores candidate and replay position");
        Check(plan.Redo() && plan.complete() && plan.candidate()[0].text==L"dependent","redo restores local history");
        recovery::AnnotationPlan keep({before},AnnotNumericPrecision::RoundTrip17,{Update(expected,after),Update(after,next)});
        Check(keep.Resolve(recovery::Choice::Keep),"explicit keep allowed");
        Check(!keep.Advance() && keep.cursor()==1 && keep.candidate()[0].text==L"current","keep cannot silently apply dependent successor");
        auto invalid=after; invalid.x1=std::numeric_limits<double>::quiet_NaN();
        Check(!keep.Replace({invalid}) && keep.candidate()[0].text==L"current","invalid manual replacement is transactional");
        invalid=before; invalid.id.clear();
        Check(!keep.Replace({invalid}),"manual annotation needs identity");
        recovery::AnnotationPlan missing({},AnnotNumericPrecision::RoundTrip17,{Update(expected,after)});
        Check(!missing.Resolve(recovery::Choice::Apply) && missing.candidate().empty(),"missing identity never redirected to another annotation");
        AnnotCommand add; add.kind=AnnotCommandKind::Add; add.after=before; add.afterIndex=0;
        recovery::AnnotationPlan duplicate({before},AnnotNumericPrecision::RoundTrip17,{add});
        Check(!duplicate.Advance() && !duplicate.Resolve(recovery::Choice::Apply),"duplicate identity cannot be inserted");
        AnnotCommand clear; clear.kind=AnnotCommandKind::ClearAll; clear.snapshot={expected};
        recovery::AnnotationPlan removal({before},AnnotNumericPrecision::RoundTrip17,{clear});
        Check(!removal.Advance() && !removal.candidate().empty(),"bulk deletion still requires review");
        Check(removal.Resolve(recovery::Choice::Keep) && !removal.candidate().empty(),"keep protects bulk deletion content");
        auto precise=before; precise.x1=1.23456789012345;
        annot_stage_replay::State provenance(1,AnnotNumericPrecision::Legacy12);
        std::vector<Annotation> upgraded{before};
        Check(provenance.Apply(&upgraded,Update(before,precise)),"modern update upgrades precision provenance");
        auto changedExpected=precise; changedExpected.x1+=1e-13;
        recovery::AnnotationPlan inherited(upgraded,AnnotNumericPrecision::Legacy12,
            {Update(changedExpected,after)},provenance);
        Check(!inherited.Advance(),"recovery must not inherit checkpoint tolerance for a modern annotation");
        std::string note=u8"本日の記録\r\n😀";
        const auto original=note;
        recovery::NoteEdit edit{{0},u8"昨日",u8"明日"};
        Check(!recovery::ApplyNoteEdit(&note,edit) && note==original,"note conflict leaves bytes unchanged");
        Check(!recovery::ResolveNoteEdit(&note,edit,{0},recovery::Choice::Apply) && note==original,"explicit location never deletes different text");
        Check(recovery::ResolveNoteEdit(&note,edit,{0},recovery::Choice::Keep) && note==original,"note keep retains original");
        recovery::NoteEdit split{{1},"","x"};
        Check(!recovery::ApplyNoteEdit(&note,split) && note==original,"UTF-8 codepoint cannot be split");
        recovery::NoteEdit replace{{0},u8"本日",u8"明日"};
        Check(recovery::ApplyNoteEdit(&note,replace) && note==u8"明日の記録\r\n😀","Unicode edit preserves CRLF and supplementary character");
        recovery::NoteEdit outside{{9999},"","x"};
        Check(!recovery::ApplyNoteEdit(&note,outside),"range bounds are checked");
        std::vector<AnnotCommand> connected;
        auto external=Text(L"external"); external.id=L"external-only";
        Check(recovery_demo::AnnotationReconnect({before},{after},{expected,external},&connected),"annotation reconnect finds candidate changes");
        recovery::AnnotationPlan merged({expected,external},AnnotNumericPrecision::RoundTrip17,connected);
        Check(!merged.Advance() && merged.Resolve(recovery::Choice::Apply) && merged.candidate().size()==2 &&
            merged.candidate()[1].id==external.id,"conflict choice preserves independent external annotation");
        Check(recovery_demo::AnnotationReconnect({before},{after},{after,external},&connected) && connected.empty(),"already-applied annotation is not duplicated");
        Check(!recovery_demo::AnnotationReconnect({before},{external,before},{before},&connected),"unsupported annotation insertion order is refused");
        Check(recovery_demo::AnnotationReconnect({before},{after},{external},&connected),"external deletion is represented for review");
        recovery::AnnotationPlan deleted({external},AnnotNumericPrecision::RoundTrip17,connected);
        Check(!deleted.Advance() && !deleted.Resolve(recovery::Choice::Apply),"external deletion is never redirected to another identity");
        std::vector<recovery::NoteEdit> edits;
        const std::string ancestor=u8"題名\r\n本文😀\r\n", ours=ancestor+u8"追加\r\n", theirs=u8"外部の題名\r\n本文😀\r\n";
        Check(recovery_demo::NoteReconnect(ancestor,ours,theirs,&edits) && edits.size()==1,"disjoint Unicode note changes reconnect");
        auto combined=theirs;
        Check(recovery::ApplyNoteEdit(&combined,edits[0]) && combined==theirs+u8"追加\r\n","translated byte offset preserves external title and candidate tail");
        Check(recovery_demo::NoteReconnect(ancestor,ours,ancestor+u8"外部\r\n",&edits) && edits.size()==1,"overlapping append requires review");
        combined=ancestor+u8"外部\r\n";
        Check(!recovery::ApplyNoteEdit(&combined,edits[0]) && combined==ancestor+u8"外部\r\n","overlapping text is never automatically replaced");
        Check(recovery_demo::NoteReconnect(ancestor,ours,ours,&edits) && edits.empty(),"already-applied note produces no operation");
        Check(!recovery_demo::NoteReconnect(ancestor,std::string("\xff"),theirs,&edits),"invalid UTF-8 reconnection is refused");
        std::vector<Annotation> final{before,external};
        Check(recovery_demo::TransferAnnotations(&final,{after}) && final.size()==3 && AnnotationEquals(final[0],before) &&
            AnnotationEquals(final[1],external) && final[2].text==after.text && final[2].id!=before.id,"drop appends content and preserves all original annotations");
        const auto firstCopy=final[2];
        Check(recovery_demo::TransferAnnotations(&final,{after}) && final.size()==4 && AnnotationEquals(final[2],firstCopy) &&
            final[3].id!=firstCopy.id,"repeated drop adds another independently identified copy");
        auto newAnnotation=after; newAnnotation.id=L"new";
        Check(recovery_demo::TransferAnnotations(&final,{after,newAnnotation}) && final.size()==6 &&
            final[4].text==after.text && final[5].id==newAnnotation.id,"group drop appends every source annotation");
        auto invalidDrop=newAnnotation; invalidDrop.x1=std::numeric_limits<double>::quiet_NaN();
        Check(!recovery_demo::TransferAnnotations(&final,{invalidDrop}) && final.size()==6,"invalid drop is transactional");
        auto reserved=external; reserved.id=L"recovery-copy-1";
        auto reservedSource=newAnnotation; reservedSource.id=L"recovery-copy-2";
        std::vector<Annotation> collisions{before,reserved};
        const std::vector<Annotation> incoming{after,reservedSource};
        Check(recovery_demo::TransferAnnotations(&collisions,incoming) && collisions.size()==4 &&
            collisions[2].id==L"recovery-copy-3" && collisions[3].id==reservedSource.id,"copy identities avoid both existing and incoming identities");
        auto expectedCopy=after; expectedCopy.id=collisions[2].id;
        Check(AnnotationEquals(expectedCopy,collisions[2]) && incoming[0].id==after.id,"copy preserves geometry and leaves the failed source unchanged");
        Check(recovery_demo::TransferAnnotations(&collisions,{}) && collisions.size()==4,"empty drop leaves original content intact");
        const std::vector<std::pair<Annotation::Type,const wchar_t*>> kinds={
            {Annotation::Type::MarkerText,L"text_marker"},{Annotation::Type::TextColor,L"text_color"},
            {Annotation::Type::MarkerFree,L"free_marker"},{Annotation::Type::TextBox,L"text"},
            {Annotation::Type::Line,L"line"},{Annotation::Type::Arrow,L"arrow"},{Annotation::Type::Wave,L"wave"},
            {Annotation::Type::Freehand,L"freehand"},{Annotation::Type::LinkMarker,L"link_marker"},
            {Annotation::Type::MathBox,L"math"},{Annotation::Type::Shape,L"shape"}};
        for (const auto& kind:kinds) {
            auto sample=before; sample.type=kind.first; sample.pageIndex=1;
            sample.path={{1,2},{3,4}}; sample.quads={1,2,3,4,5,6,7,8}; sample.dash={3,2};
            sample.shapeKind=ShapeKind::Ellipse; sample.shapeDrawMode=ShapeDrawMode::Outline;
            sample.mathKind=MathKind::Latex; sample.text=L"a/b";
            std::vector<Annotation> copies{sample};
            const auto label=recovery_demo::AnnotationSummary(sample);
            Check(label.find(std::wstring(L"readonly.clrop.kind.")+kind.second)!=std::wstring::npos &&
                label.find(L"recovery.demo.page_prefix2")!=std::wstring::npos &&
                recovery_demo::TransferAnnotations(&copies,{sample}) && copies.size()==2 &&
                AnnotationEquals(copies[0],sample) && AnnotationEquals(copies[1],sample) &&
                copies[0].id!=copies[1].id,"each annotation kind is identifiable and copied without changing its content");
        }
        recovery_demo::Case sourceCase; sourceCase.savedAnnotations={before,external}; sourceCase.inspection.annotations={expected,external};
        sourceCase.inspection.remainingAnnotations={Update(expected,after)};
        const auto sources=recovery_demo::CompositionSources(sourceCase);
        Check(sources.size()==3 && sources[1].annotations.size()==2 && sources[1].annotations[0].text==after.text &&
            sources[2].annotations[0].text==expected.text && sourceCase.inspection.annotations[0].text==expected.text,"failed-file source keeps complete group and earlier protected content");
        std::cout<<"Recovery model: "<<checks<<" checks, 0 failures\n"; return 0;
    } catch(const std::exception& exception) { std::cerr<<exception.what()<<'\n'; return 1; }
}
