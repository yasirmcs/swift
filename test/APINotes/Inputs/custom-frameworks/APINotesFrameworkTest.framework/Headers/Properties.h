#pragma clang assume_nonnull begin

__attribute__((objc_root_class))
@interface Base
@end

@interface TestProperties: Base
@property (nonatomic, readwrite, retain) id accessorsOnly;
@property (nonatomic, readwrite, retain, class) id accessorsOnlyForClass;

@property (nonatomic, readonly, retain) id accessorsOnlyRO;
@property (nonatomic, readwrite, weak) id accessorsOnlyWeak;

@property (nonatomic, readwrite, retain) id accessorsOnlyInVersion3;
@property (nonatomic, readwrite, retain, class) id accessorsOnlyForClassInVersion3;

@property (nonatomic, readwrite, retain) id accessorsOnlyExceptInVersion3;
@property (nonatomic, readwrite, retain, class) id accessorsOnlyForClassExceptInVersion3;
@end

@interface TestPropertiesSub: TestProperties
@property (nonatomic, readwrite, retain) id accessorsOnly;
@property (nonatomic, readwrite, retain, class) id accessorsOnlyForClass;
@end

@interface TestProperties (Retyped)
@property (nonatomic, readwrite, retain) id accessorsOnlyWithNewType;
@end

#pragma clang assume_nonnull end
